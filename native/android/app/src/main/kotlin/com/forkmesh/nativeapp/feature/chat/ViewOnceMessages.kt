package com.forkmesh.nativeapp.feature.chat

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.MediaMetadataRetriever
import android.media.MediaPlayer
import android.media.MediaRecorder
import android.os.Build
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.VideoView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.SecureFlagPolicy
import androidx.core.content.ContextCompat
import com.forkmesh.nativeapp.chat.MessageCaptureFilters
import com.forkmesh.nativeapp.chat.PassThroughCaptureFilters
import com.forkmesh.nativeapp.chat.ViewOnceCodec
import com.forkmesh.nativeapp.chat.ViewOnceEntry
import com.forkmesh.nativeapp.chat.ViewOnceException
import com.forkmesh.nativeapp.chat.ViewOnceGone
import com.forkmesh.nativeapp.chat.ViewOnceKind
import com.forkmesh.nativeapp.chat.ViewOnceState
import com.forkmesh.nativeapp.feature.composer.ComposerIconButton
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.io.File
import java.util.UUID
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object ViewOnceCapture {
    const val VIDEO_MIME = "video/mp4"
    const val VOICE_MIME = "audio/mp4"
    const val VIDEO_WIDTH = 640
    const val VIDEO_HEIGHT = 480
    const val VIDEO_BITRATE = 1_000_000
    const val VIDEO_FPS = 24
    const val VIDEO_AUDIO_BITRATE = 64_000
    const val VOICE_BITRATE = 32_000
    const val VOICE_SAMPLE_RATE = 22_050
    const val WAVE_BARS = 48
    const val SCREEN_NOTE =
        "Plays once, then it is deleted. Screenshots and screen recording are blocked on most phones, but that cannot be guaranteed."

    @Volatile
    var filters: MessageCaptureFilters = PassThroughCaptureFilters

    fun mime(kind: ViewOnceKind): String = if (kind == ViewOnceKind.VIDEO) VIDEO_MIME else VOICE_MIME

    fun scratch(context: Context, suffix: String): File {
        val dir = File(context.cacheDir, "view-once").apply { mkdirs() }
        return File(dir, "${UUID.randomUUID()}$suffix")
    }

    fun durationOf(file: File, fallback: Long): Long {
        val retriever = MediaMetadataRetriever()
        return try {
            retriever.setDataSource(file.absolutePath)
            retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: fallback
        } catch (_: Exception) {
            fallback
        } finally {
            runCatching { retriever.release() }
        }
    }

    fun recorder(context: Context): MediaRecorder =
        if (Build.VERSION.SDK_INT >= 31) MediaRecorder(context) else @Suppress("DEPRECATION") MediaRecorder()
}

@Suppress("DEPRECATION")
private class VideoMessageCapture(private val context: Context) {
    private var camera: android.hardware.Camera? = null
    private var recorder: MediaRecorder? = null
    private var videoWidth = ViewOnceCapture.VIDEO_WIDTH
    private var videoHeight = ViewOnceCapture.VIDEO_HEIGHT
    private var hint = 270
    var limitReached = false
        private set

    private fun pick(sizes: List<android.hardware.Camera.Size>?): Pair<Int, Int>? {
        if (sizes.isNullOrEmpty()) return null
        sizes.firstOrNull { it.width == ViewOnceCapture.VIDEO_WIDTH && it.height == ViewOnceCapture.VIDEO_HEIGHT }?.let { return it.width to it.height }
        val budget = ViewOnceCapture.VIDEO_WIDTH * ViewOnceCapture.VIDEO_HEIGHT
        val fits = sizes.filter { it.width * it.height <= budget && it.width >= it.height }
        val chosen = fits.maxByOrNull { it.width * it.height } ?: sizes.minByOrNull { it.width * it.height } ?: return null
        return chosen.width to chosen.height
    }

    fun preview(holder: SurfaceHolder): Boolean = try {
        var cam = camera
        if (cam == null) {
            val info = android.hardware.Camera.CameraInfo()
            var chosen = -1
            for (index in 0 until android.hardware.Camera.getNumberOfCameras()) {
                android.hardware.Camera.getCameraInfo(index, info)
                if (info.facing == android.hardware.Camera.CameraInfo.CAMERA_FACING_FRONT) {
                    chosen = index
                    break
                }
            }
            if (chosen < 0 && android.hardware.Camera.getNumberOfCameras() > 0) chosen = 0
            if (chosen < 0) throw IllegalStateException("no camera")
            android.hardware.Camera.getCameraInfo(chosen, info)
            cam = android.hardware.Camera.open(chosen)
            val front = info.facing == android.hardware.Camera.CameraInfo.CAMERA_FACING_FRONT
            hint = info.orientation
            cam.setDisplayOrientation(if (front) (360 - info.orientation) % 360 else info.orientation)
            val params = cam.parameters
            pick(params.supportedVideoSizes ?: params.supportedPreviewSizes)?.let { (w, h) ->
                videoWidth = w
                videoHeight = h
            }
            pick(params.supportedPreviewSizes)?.let { (w, h) -> params.setPreviewSize(w, h) }
            if (params.supportedFocusModes?.contains(android.hardware.Camera.Parameters.FOCUS_MODE_CONTINUOUS_VIDEO) == true) {
                params.focusMode = android.hardware.Camera.Parameters.FOCUS_MODE_CONTINUOUS_VIDEO
            }
            params.setRecordingHint(true)
            runCatching { cam.parameters = params }
            camera = cam
        }
        cam!!.setPreviewDisplay(holder)
        cam.startPreview()
        true
    } catch (_: Exception) {
        release()
        false
    }

    fun start(file: File, holder: SurfaceHolder, onLimit: () -> Unit): Boolean {
        val cam = camera ?: return false
        limitReached = false
        val next = ViewOnceCapture.recorder(context)
        return try {
            cam.unlock()
            next.setCamera(cam)
            next.setAudioSource(MediaRecorder.AudioSource.CAMCORDER)
            next.setVideoSource(MediaRecorder.VideoSource.CAMERA)
            next.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
            next.setVideoEncoder(MediaRecorder.VideoEncoder.H264)
            next.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
            next.setVideoSize(videoWidth, videoHeight)
            next.setVideoFrameRate(ViewOnceCapture.VIDEO_FPS)
            next.setVideoEncodingBitRate(ViewOnceCapture.VIDEO_BITRATE)
            next.setAudioEncodingBitRate(ViewOnceCapture.VIDEO_AUDIO_BITRATE)
            next.setAudioSamplingRate(44_100)
            next.setAudioChannels(1)
            next.setOrientationHint(hint)
            next.setMaxDuration((ViewOnceKind.VIDEO.maxDurationMs + 500).toInt())
            next.setMaxFileSize(ViewOnceKind.VIDEO.maxBytes.toLong() - 256 * 1024)
            next.setOnInfoListener { _, what, _ ->
                if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_DURATION_REACHED || what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED) {
                    limitReached = true
                    onLimit()
                }
            }
            next.setOutputFile(file.absolutePath)
            next.setPreviewDisplay(holder.surface)
            next.prepare()
            next.start()
            recorder = next
            true
        } catch (_: Exception) {
            runCatching { next.reset() }
            next.release()
            runCatching { cam.reconnect() }
            false
        }
    }

    fun stop(): Boolean {
        val current = recorder ?: return false
        recorder = null
        val stopped = runCatching { current.stop() }.isSuccess
        runCatching { current.reset() }
        current.release()
        runCatching { camera?.reconnect() }
        return stopped || limitReached
    }

    fun release() {
        recorder?.let {
            runCatching { it.reset() }
            it.release()
        }
        recorder = null
        camera?.let {
            runCatching { it.stopPreview() }
            it.release()
        }
        camera = null
    }
}

private class VoiceMessageCapture(private val context: Context) {
    private var recorder: MediaRecorder? = null
    var limitReached = false
        private set

    fun start(file: File, onLimit: () -> Unit): Boolean {
        limitReached = false
        val next = ViewOnceCapture.recorder(context)
        return try {
            next.setAudioSource(MediaRecorder.AudioSource.MIC)
            next.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
            next.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
            next.setAudioEncodingBitRate(ViewOnceCapture.VOICE_BITRATE)
            next.setAudioSamplingRate(ViewOnceCapture.VOICE_SAMPLE_RATE)
            next.setAudioChannels(1)
            next.setMaxDuration((ViewOnceKind.VOICE.maxDurationMs + 500).toInt())
            next.setOnInfoListener { _, what, _ ->
                if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_DURATION_REACHED) {
                    limitReached = true
                    onLimit()
                }
            }
            next.setOutputFile(file.absolutePath)
            next.prepare()
            next.start()
            recorder = next
            true
        } catch (_: Exception) {
            runCatching { next.reset() }
            next.release()
            false
        }
    }

    fun level(): Float {
        val amplitude = runCatching { recorder?.maxAmplitude ?: 0 }.getOrDefault(0)
        return (amplitude / 32767f).coerceIn(0f, 1f)
    }

    fun stop(): Boolean {
        val current = recorder ?: return false
        recorder = null
        val stopped = runCatching { current.stop() }.isSuccess
        runCatching { current.reset() }
        current.release()
        return stopped || limitReached
    }

    fun release() {
        recorder?.let {
            runCatching { it.reset() }
            it.release()
        }
        recorder = null
    }
}

private enum class RecordPhase { Ready, Recording, Review, Sending }

@Composable
fun ViewOnceComposerButtons(conversation: String) {
    var kind by remember(conversation) { mutableStateOf<ViewOnceKind?>(null) }
    ComposerIconButton(icon = "videocam", description = "Record a view-once video message", onClick = { kind = ViewOnceKind.VIDEO })
    ComposerIconButton(icon = "mic", description = "Record a view-once voice message", onClick = { kind = ViewOnceKind.VOICE })
    kind?.let { current -> ViewOnceRecorderDialog(conversation, current) { kind = null } }
}

private fun permissionsFor(kind: ViewOnceKind): Array<String> =
    if (kind == ViewOnceKind.VIDEO) arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
    else arrayOf(Manifest.permission.RECORD_AUDIO)

private fun granted(context: Context, kind: ViewOnceKind): Boolean =
    permissionsFor(kind).all { ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED }

@Composable
private fun ViewOnceRecorderDialog(conversation: String, kind: ViewOnceKind, onClose: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var allowed by remember { mutableStateOf(granted(context, kind)) }
    var asked by remember { mutableStateOf(false) }
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        asked = true
        allowed = granted(context, kind)
    }
    LaunchedEffect(Unit) { if (!allowed) permission.launch(permissionsFor(kind)) }
    val video = remember { VideoMessageCapture(context) }
    val voice = remember { VoiceMessageCapture(context) }
    var holder by remember { mutableStateOf<SurfaceHolder?>(null) }
    var phase by remember { mutableStateOf(RecordPhase.Ready) }
    var file by remember { mutableStateOf<File?>(null) }
    var startedAt by remember { mutableLongStateOf(0L) }
    var elapsed by remember { mutableLongStateOf(0L) }
    var durationMs by remember { mutableLongStateOf(0L) }
    var error by remember { mutableStateOf("") }
    var progress by remember { mutableStateOf("") }
    val levels = remember { mutableStateListOf<Float>() }

    fun discardFile() {
        file?.delete()
        file = null
    }

    fun stopRecording() {
        if (phase != RecordPhase.Recording) return
        val ok = if (kind == ViewOnceKind.VIDEO) video.stop() else voice.stop()
        val recorded = file
        val measured = (System.currentTimeMillis() - startedAt).coerceAtMost(kind.maxDurationMs)
        if (!ok || recorded == null || !recorded.exists() || recorded.length() == 0L) {
            discardFile()
            error = "The recording was too short. Try again."
            phase = RecordPhase.Ready
            holder?.let { if (kind == ViewOnceKind.VIDEO) video.preview(it) }
            return
        }
        durationMs = ViewOnceCapture.durationOf(recorded, measured).coerceIn(0, kind.maxDurationMs)
        if (kind == ViewOnceKind.VIDEO) video.release()
        phase = RecordPhase.Review
    }

    fun startRecording() {
        error = ""
        discardFile()
        levels.clear()
        val target = ViewOnceCapture.scratch(context, if (kind == ViewOnceKind.VIDEO) ".mp4" else ".m4a")
        val started = if (kind == ViewOnceKind.VIDEO) {
            val surface = holder
            surface != null && video.start(target, surface) { scope.launch { stopRecording() } }
        } else {
            voice.start(target) { scope.launch { stopRecording() } }
        }
        if (!started) {
            target.delete()
            error = if (kind == ViewOnceKind.VIDEO) "The camera could not start recording." else "The microphone could not start recording."
            return
        }
        file = target
        startedAt = System.currentTimeMillis()
        elapsed = 0
        phase = RecordPhase.Recording
    }

    fun close() {
        video.release()
        voice.release()
        discardFile()
        onClose()
    }

    LaunchedEffect(phase) {
        while (phase == RecordPhase.Recording) {
            elapsed = System.currentTimeMillis() - startedAt
            if (kind == ViewOnceKind.VOICE) {
                levels.add(voice.level())
                while (levels.size > ViewOnceCapture.WAVE_BARS) levels.removeAt(0)
            }
            if (elapsed >= kind.maxDurationMs) {
                stopRecording()
                break
            }
            delay(80)
        }
    }

    DisposableEffect(Unit) {
        onDispose {
            video.release()
            voice.release()
            file?.delete()
        }
    }

    fun send() {
        val recorded = file ?: return
        phase = RecordPhase.Sending
        error = ""
        progress = "Encrypting"
        scope.launch {
            val result = withContext(Dispatchers.IO) {
                try {
                    val mime = ViewOnceCapture.mime(kind)
                    val filtered = ViewOnceCapture.filters.apply(kind, recorded, mime)
                    val bytes = filtered.readBytes()
                    if (filtered != recorded) filtered.delete()
                    val service = ChatDepth.direct ?: return@withContext "Direct messages are not connected."
                    val sent = service.sendViewOnce(conversation, kind, mime, durationMs, bytes) { done, total ->
                        scope.launch { progress = "Uploading $done of $total" }
                    }
                    bytes.fill(0)
                    if (sent) "" else "The message could not be sent. Reconnecting..."
                } catch (failure: ViewOnceException) {
                    failure.message ?: "The message could not be sent."
                } catch (_: Exception) {
                    "The message could not be sent."
                }
            }
            if (result.isEmpty()) {
                discardFile()
                ChatDepth.bump()
                onClose()
            } else {
                error = result
                phase = RecordPhase.Review
            }
        }
    }

    Dialog(
        onDismissRequest = { if (phase != RecordPhase.Sending) close() },
        properties = DialogProperties(usePlatformDefaultWidth = false, dismissOnClickOutside = false),
    ) {
        Column(
            Modifier.fillMaxSize().background(Color.Black).padding(FmSpace.x4),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    if (kind == ViewOnceKind.VIDEO) "View-once video" else "View-once voice message",
                    modifier = Modifier.weight(1f),
                    style = Fm.text.titleMedium.copy(color = Color.White),
                )
                FmIconButton(icon = "close", label = "Discard and close", color = Color.White, onPressed = { if (phase != RecordPhase.Sending) close() })
            }
            Text(
                "The other person can play it once. It is deleted after that, or after 7 days.",
                modifier = Modifier.fillMaxWidth(),
                style = Fm.text.bodySmall.copy(color = Color.White.copy(alpha = 0.7f)),
            )
            Spacer(Modifier.height(FmSpace.x3))
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                when {
                    !allowed -> PermissionNote(kind, asked) { permission.launch(permissionsFor(kind)) }
                    phase == RecordPhase.Review || phase == RecordPhase.Sending -> file?.let { ReviewPlayer(kind, it, durationMs) }
                    kind == ViewOnceKind.VIDEO -> Box(
                        Modifier.fillMaxWidth().aspectRatio(3f / 4f).clip(RoundedCornerShape(FmRadius.md)).background(Color.DarkGray)
                            .semantics { contentDescription = "Front camera preview" },
                    ) {
                        AndroidView(
                            factory = { ctx ->
                                SurfaceView(ctx).apply {
                                    this.holder.addCallback(object : SurfaceHolder.Callback {
                                        override fun surfaceCreated(created: SurfaceHolder) {
                                            holder = created
                                            if (!video.preview(created)) error = "The camera could not be opened."
                                        }

                                        override fun surfaceChanged(changed: SurfaceHolder, format: Int, width: Int, height: Int) {}

                                        override fun surfaceDestroyed(destroyed: SurfaceHolder) {
                                            if (phase == RecordPhase.Recording) video.stop()
                                            holder = null
                                            video.release()
                                        }
                                    })
                                }
                            },
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                    else -> Waveform(levels, Modifier.fillMaxWidth().height(120.dp))
                }
            }
            Spacer(Modifier.height(FmSpace.x3))
            val limit = ViewOnceCodec.durationLabel(kind.maxDurationMs)
            val clock = when (phase) {
                RecordPhase.Recording -> "${ViewOnceCodec.durationLabel(elapsed)} / $limit · ${ViewOnceCodec.durationLabel(kind.maxDurationMs - elapsed)} left"
                RecordPhase.Review, RecordPhase.Sending -> ViewOnceCodec.durationLabel(durationMs)
                RecordPhase.Ready -> "Up to $limit"
            }
            Text(
                clock,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                style = Fm.text.titleSmall.copy(color = if (phase == RecordPhase.Recording) Fm.colors.danger else Color.White),
            )
            if (error.isNotEmpty()) {
                Text(
                    error,
                    modifier = Modifier.padding(top = FmSpace.x1).semantics { liveRegion = LiveRegionMode.Assertive },
                    style = Fm.text.bodySmall.copy(color = Fm.colors.danger),
                    textAlign = TextAlign.Center,
                )
            }
            if (phase == RecordPhase.Sending) {
                Text(progress, modifier = Modifier.padding(top = FmSpace.x1), style = Fm.text.bodySmall.copy(color = Color.White))
            }
            Spacer(Modifier.height(FmSpace.x3))
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3), verticalAlignment = Alignment.CenterVertically) {
                when (phase) {
                    RecordPhase.Ready -> RecordButton(
                        recording = false,
                        enabled = allowed && (kind == ViewOnceKind.VOICE || holder != null),
                        label = if (kind == ViewOnceKind.VIDEO) "Start recording video" else "Start recording voice",
                        onTap = ::startRecording,
                    )
                    RecordPhase.Recording -> RecordButton(recording = true, enabled = true, label = "Stop recording", onTap = ::stopRecording)
                    RecordPhase.Review -> {
                        FmToneButton(label = "Discard", icon = "delete_outline", tone = FmTone.Danger, onPressed = ::close, height = 44.dp)
                        FmToneButton(
                            label = "Record again",
                            icon = "replay",
                            onPressed = {
                                discardFile()
                                error = ""
                                phase = RecordPhase.Ready
                            },
                            height = 44.dp,
                        )
                        FmToneButton(label = "Send", icon = "send", tone = FmTone.Accent, filled = true, onPressed = ::send, height = 44.dp)
                    }
                    RecordPhase.Sending -> FmToneButton(label = "Sending", icon = "send", onPressed = null, height = 44.dp)
                }
            }
        }
    }
}

@Composable
private fun PermissionNote(kind: ViewOnceKind, asked: Boolean, onAsk: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            if (kind == ViewOnceKind.VIDEO) "Allow the camera and microphone to record a video message." else "Allow the microphone to record a voice message.",
            style = Fm.text.bodyMedium.copy(color = Color.White),
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(FmSpace.x2))
        FmToneButton(label = if (asked) "Ask again" else "Allow", onPressed = onAsk, tone = FmTone.Accent, height = 44.dp)
    }
}

@Composable
private fun RecordButton(recording: Boolean, enabled: Boolean, label: String, onTap: () -> Unit) {
    Box(
        Modifier
            .size(72.dp)
            .clip(CircleShape)
            .background(Color.White.copy(alpha = if (enabled) 1f else 0.4f))
            .clickable(enabled = enabled, onClick = onTap)
            .semantics {
                role = Role.Button
                contentDescription = label
            },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier
                .size(if (recording) 28.dp else 56.dp)
                .clip(if (recording) RoundedCornerShape(6.dp) else CircleShape)
                .background(Fm.colors.danger),
        )
    }
}

@Composable
private fun Waveform(levels: List<Float>, modifier: Modifier) {
    val bar = Fm.colors.accent
    Canvas(modifier.semantics { contentDescription = "Microphone level" }) {
        val count = ViewOnceCapture.WAVE_BARS
        val slot = size.width / count
        val width = slot * 0.6f
        val offset = count - levels.size
        for (index in 0 until count) {
            val level = levels.getOrNull(index - offset) ?: 0f
            val height = (size.height * (0.06f + 0.94f * level)).coerceAtLeast(2f)
            drawRoundRect(
                color = bar,
                topLeft = Offset(index * slot + (slot - width) / 2, (size.height - height) / 2),
                size = Size(width, height),
                cornerRadius = CornerRadius(width / 2, width / 2),
            )
        }
    }
}

@Composable
private fun ReviewPlayer(kind: ViewOnceKind, file: File, durationMs: Long) {
    if (kind == ViewOnceKind.VIDEO) {
        var view by remember { mutableStateOf<VideoView?>(null) }
        Box(Modifier.fillMaxWidth().aspectRatio(3f / 4f).clip(RoundedCornerShape(FmRadius.md)).background(Color.DarkGray)) {
            AndroidView(
                factory = { ctx ->
                    VideoView(ctx).apply {
                        setVideoPath(file.absolutePath)
                        setOnPreparedListener { it.isLooping = false }
                        view = this
                    }
                },
                onRelease = { it.stopPlayback() },
                modifier = Modifier.fillMaxSize().semantics { contentDescription = "Your video message, ${ViewOnceCodec.durationLabel(durationMs)}" },
            )
            FmIconButton(
                icon = "play_arrow",
                label = "Play your video",
                color = Color.White,
                modifier = Modifier.align(Alignment.BottomStart),
                onPressed = {
                    view?.let {
                        it.seekTo(0)
                        it.start()
                    }
                },
            )
        }
    } else {
        var player by remember { mutableStateOf<MediaPlayer?>(null) }
        DisposableEffect(file) {
            onDispose {
                player?.release()
                player = null
            }
        }
        Row(
            Modifier.clip(RoundedCornerShape(FmRadius.md)).background(Color.DarkGray).padding(FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIconButton(
                icon = if (player == null) "play_arrow" else "stop",
                label = if (player == null) "Play your voice message" else "Stop",
                color = Color.White,
                onPressed = {
                    val current = player
                    if (current != null) {
                        current.release()
                        player = null
                    } else {
                        runCatching {
                            val next = MediaPlayer()
                            next.setDataSource(file.absolutePath)
                            next.setOnCompletionListener {
                                it.release()
                                player = null
                            }
                            next.prepare()
                            next.start()
                            player = next
                        }
                    }
                },
            )
            Text("Voice message · ${ViewOnceCodec.durationLabel(durationMs)}", style = Fm.text.bodyMedium.copy(color = Color.White))
        }
    }
}

@Composable
fun ViewOnceBubble(entry: ViewOnceEntry) {
    val colors = Fm.colors
    var open by remember(entry.messageId) { mutableStateOf(false) }
    var busy by remember(entry.messageId) { mutableStateOf(false) }
    val tone = when (entry.state) {
        ViewOnceState.READY, ViewOnceState.SENT -> colors.accent
        ViewOnceState.OPENED -> colors.success
        else -> colors.textTertiary
    }
    val describe = if (entry.playable) "${entry.description}. Double tap to play once." else entry.description
    Row(
        Modifier
            .padding(top = FmSpace.x1)
            .widthIn(max = 280.dp)
            .clip(RoundedCornerShape(FmRadius.md))
            .background(if (entry.playable) colors.accentSubtle else colors.bgRaised)
            .clickable(enabled = entry.playable) { open = true }
            .heightIn(min = 52.dp)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2)
            .semantics(mergeDescendants = true) {
                contentDescription = describe
                if (entry.playable) role = Role.Button
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(if (entry.kind == ViewOnceKind.VIDEO) "videocam" else "mic", size = 22.dp, tint = tone)
        Spacer(Modifier.width(FmSpace.x2))
        Column(Modifier.weight(1f, fill = false)) {
            Text(entry.kind.label, style = Fm.text.bodyMedium.copy(color = colors.textPrimary))
            Text(
                "${entry.durationLabel} · ${entry.statusLabel} · plays once",
                style = Fm.text.bodySmall.copy(fontSize = FmType.micro, color = colors.textTertiary),
            )
        }
        if (entry.self && entry.state == ViewOnceState.SENT) {
            Spacer(Modifier.width(FmSpace.x1))
            FmIconButton(
                icon = "undo",
                label = "Unsend this ${entry.kind.label.lowercase()}",
                color = colors.textTertiary,
                onPressed = if (busy) null else ({
                    busy = true
                    ProjectLifecycle.scope.launch(Dispatchers.IO) {
                        val result = runCatching { ChatDepth.direct?.withdrawViewOnce(entry.messageId) }
                        result.exceptionOrNull()?.let { ChatDepth.post(it.message ?: "The message could not be unsent.") }
                        withContext(Dispatchers.Main) {
                            busy = false
                            ChatDepth.bump()
                        }
                    }
                }),
            )
        }
    }
    if (open) ViewOnceViewer(entry) { open = false }
}

private enum class ViewerPhase { Loading, Playing, Finished, Failed }

@Composable
private fun ViewOnceViewer(entry: ViewOnceEntry, onClose: () -> Unit) {
    val context = LocalContext.current
    var phase by remember { mutableStateOf(ViewerPhase.Loading) }
    var message by remember { mutableStateOf("") }
    var file by remember { mutableStateOf<File?>(null) }
    var started by remember { mutableStateOf(false) }
    var player by remember { mutableStateOf<MediaPlayer?>(null) }
    var position by remember { mutableLongStateOf(0L) }
    val messageId = entry.messageId

    fun dropCopy() {
        player?.let { runCatching { it.release() } }
        player = null
        file?.delete()
        file = null
    }

    fun begin() {
        if (started) return
        started = true
        ProjectLifecycle.scope.launch(Dispatchers.IO) {
            ChatDepth.direct?.consumeViewOnce(messageId)
            withContext(Dispatchers.Main) { ChatDepth.bump() }
        }
    }

    fun finish() {
        dropCopy()
        phase = ViewerPhase.Finished
    }

    LaunchedEffect(messageId) {
        val outcome = withContext(Dispatchers.IO) {
            try {
                val service = ChatDepth.direct ?: return@withContext Result.failure<File>(ViewOnceException("Direct messages are not connected."))
                val bytes = service.downloadViewOnce(messageId)
                val target = ViewOnceCapture.scratch(context, if (entry.kind == ViewOnceKind.VIDEO) ".mp4" else ".m4a")
                try {
                    target.writeBytes(bytes)
                } finally {
                    bytes.fill(0)
                }
                Result.success(target)
            } catch (failure: Exception) {
                Result.failure(failure)
            }
        }
        outcome.onSuccess {
            file = it
            phase = ViewerPhase.Playing
        }.onFailure {
            message = when (it) {
                is ViewOnceGone -> when (it.state) {
                    "opened" -> "This message was already opened."
                    "withdrawn" -> "The sender unsent this message."
                    else -> "This message expired."
                }
                else -> it.message ?: "The message could not be loaded."
            }
            phase = ViewerPhase.Failed
            ChatDepth.bump()
        }
    }

    LaunchedEffect(phase, player) {
        while (phase == ViewerPhase.Playing && player != null) {
            position = runCatching { player?.currentPosition?.toLong() ?: 0L }.getOrDefault(0L)
            delay(200)
        }
    }

    DisposableEffect(messageId) {
        onDispose { dropCopy() }
    }

    Dialog(
        onDismissRequest = {
            dropCopy()
            onClose()
        },
        properties = DialogProperties(usePlatformDefaultWidth = false, securePolicy = SecureFlagPolicy.SecureOn),
    ) {
        Column(
            Modifier.fillMaxSize().background(Color.Black).padding(FmSpace.x4),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(entry.kind.label, modifier = Modifier.weight(1f), style = Fm.text.titleMedium.copy(color = Color.White))
                FmIconButton(
                    icon = "close",
                    label = if (started) "Close. The message is gone." else "Close without playing",
                    color = Color.White,
                    onPressed = {
                        dropCopy()
                        onClose()
                    },
                )
            }
            Text(ViewOnceCapture.SCREEN_NOTE, modifier = Modifier.fillMaxWidth(), style = Fm.text.bodySmall.copy(color = Color.White.copy(alpha = 0.7f)))
            Spacer(Modifier.height(FmSpace.x3))
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                val playing = file
                when {
                    phase == ViewerPhase.Loading -> Text(
                        "Downloading and decrypting...",
                        modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                        style = Fm.text.bodyMedium.copy(color = Color.White),
                    )
                    phase == ViewerPhase.Failed -> Text(
                        message,
                        modifier = Modifier.semantics { liveRegion = LiveRegionMode.Assertive },
                        style = Fm.text.bodyMedium.copy(color = Color.White),
                        textAlign = TextAlign.Center,
                    )
                    phase == ViewerPhase.Finished -> Text(
                        "Opened. This message has been deleted.",
                        modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                        style = Fm.text.bodyMedium.copy(color = Color.White),
                        textAlign = TextAlign.Center,
                    )
                    playing != null && entry.kind == ViewOnceKind.VIDEO -> AndroidView(
                        factory = { ctx ->
                            VideoView(ctx).apply {
                                setOnPreparedListener { media ->
                                    media.isLooping = false
                                    start()
                                    begin()
                                }
                                setOnCompletionListener { finish() }
                                setOnErrorListener { _, _, _ ->
                                    message = "This video could not be played."
                                    dropCopy()
                                    phase = ViewerPhase.Failed
                                    true
                                }
                                setVideoPath(playing.absolutePath)
                            }
                        },
                        onRelease = { it.stopPlayback() },
                        modifier = Modifier.fillMaxWidth().aspectRatio(3f / 4f)
                            .semantics { contentDescription = "Video message from the sender, playing once" },
                    )
                    playing != null -> {
                        DisposableEffect(playing) {
                            val next = MediaPlayer()
                            runCatching {
                                next.setDataSource(playing.absolutePath)
                                next.setOnCompletionListener { finish() }
                                next.setOnErrorListener { _, _, _ ->
                                    message = "This voice message could not be played."
                                    dropCopy()
                                    phase = ViewerPhase.Failed
                                    true
                                }
                                next.setOnPreparedListener {
                                    it.start()
                                    begin()
                                }
                                next.prepareAsync()
                                player = next
                            }.onFailure {
                                next.release()
                                message = "This voice message could not be played."
                                dropCopy()
                                phase = ViewerPhase.Failed
                            }
                            onDispose {}
                        }
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            FmIcon("graphic_eq", size = 64.dp, tint = Fm.colors.accent)
                            Spacer(Modifier.height(FmSpace.x2))
                            Text(
                                "${ViewOnceCodec.durationLabel(position)} / ${entry.durationLabel}",
                                modifier = Modifier.semantics { contentDescription = "Voice message playing once" },
                                style = Fm.text.titleSmall.copy(color = Color.White),
                            )
                        }
                    }
                }
            }
            if (phase == ViewerPhase.Finished || phase == ViewerPhase.Failed) {
                FmToneButton(label = "Close", onPressed = onClose, tone = FmTone.Accent, height = 44.dp)
            }
        }
    }
}
