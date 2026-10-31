package com.forkmesh.nativeapp.feature.chat

import android.media.AudioAttributes
import android.media.MediaPlayer
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.VideoView
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.facechanger.FaceChangerLook
import com.forkmesh.nativeapp.feature.facechanger.CallRoundButton
import com.forkmesh.nativeapp.feature.facechanger.FaceChangerButtons
import com.forkmesh.nativeapp.feature.facechanger.FaceChangerFeature
import com.forkmesh.nativeapp.feature.facechanger.adoptContext
import com.forkmesh.nativeapp.feature.facechanger.callHeadphonesConnected
import com.forkmesh.nativeapp.feature.facechanger.rememberFaceChangerView
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.io.File
import kotlinx.coroutines.delay

private enum class PreviewPhase { OPENING, READY, FAILED, BUSY }

private enum class ClipPhase { IDLE, STARTING, RECORDING, FINISHING, PLAYING, DONE }

private data class TestClip(val file: File, val video: Boolean, val durationMs: Long)

private const val TEST_CLIP_MS = 10_000L
private const val TEST_CLIP_DIR = "precall-test"
private val CALL_BACKDROP = Color(0xFF0E1116)

private fun clipExtension(mime: String): String = when (mime) {
    "video/mp4" -> ".mp4"
    "audio/mp4" -> ".m4a"
    else -> "." + mime.substringAfter('/').ifEmpty { "bin" }
}

private fun testClipFailure(error: Throwable): String = when (error.message) {
    "face_not_ready", "face_unavailable", "background_unavailable" -> "The face or background could not start here, so nothing was recorded."
    "face_not_found" -> "No face was found in your photo, so nothing was recorded."
    "voice_unavailable" -> "The changed voice could not start here, so nothing was recorded."
    "recording_unsupported" -> "This phone cannot record a test clip."
    else -> "The test clip could not start."
}

private val CLIP_AUDIO: AudioAttributes = AudioAttributes.Builder()
    .setUsage(AudioAttributes.USAGE_MEDIA)
    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
    .build()

private fun clearFolder(folder: File) {
    folder.listFiles()?.forEach { if (it.isFile) it.delete() }
}

@Composable
private fun PillButton(
    label: String,
    background: Color,
    modifier: Modifier = Modifier,
    icon: String? = null,
    enabled: Boolean = true,
    onPressed: () -> Unit,
) {
    Row(
        modifier
            .height(50.dp)
            .clip(RoundedCornerShape(25.dp))
            .background(if (enabled) background else background.copy(alpha = .4f))
            .clickable(enabled = enabled, role = Role.Button, onClick = onPressed)
            .padding(horizontal = 22.dp),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) {
            FmIcon(icon, size = 22.dp, tint = Color.White)
            Spacer(Modifier.width(FmSpace.x2))
        }
        Text(label, color = Color.White, fontWeight = FontWeight.SemiBold, maxLines = 1)
    }
}

@Composable
internal fun PreCallScreen(precall: PreCall) {
    val context = LocalContext.current
    val colors = Fm.colors
    val peers = FaceChangerFeature.peers
    val video = precall.media == DirectCallKind.VIDEO
    val look = rememberFaceChangerView()
    val folder = remember { File(context.cacheDir, TEST_CLIP_DIR) }
    var phase by remember { mutableStateOf(if (peers == null) PreviewPhase.FAILED else PreviewPhase.OPENING) }
    var clipPhase by remember { mutableStateOf(ClipPhase.IDLE) }
    var clip by remember { mutableStateOf<TestClip?>(null) }
    var playToken by remember { mutableIntStateOf(0) }
    var remainingMs by remember { mutableLongStateOf(TEST_CLIP_MS) }
    var notice by remember { mutableStateOf("") }
    var hearing by remember { mutableStateOf(false) }
    var attempt by remember { mutableIntStateOf(0) }
    var calling by remember { mutableStateOf(false) }
    val webView = peers?.view?.collectAsState()?.value
    val previewing = peers?.previewing?.collectAsState()?.value == true
    val faces = peers?.facesSupported?.collectAsState()?.value == true
    val thumbs = peers?.backgrounds?.collectAsState()?.value
    val quality = peers?.quality?.collectAsState()?.value

    fun cancel() {
        DirectCallFeature.closePreCall()
    }

    fun discard() {
        val gone = clip
        clip = null
        clipPhase = ClipPhase.IDLE
        if (gone != null) FaceChangerFeature.run { _ -> gone.file.delete() }
    }

    fun finish() {
        if (clipPhase != ClipPhase.RECORDING || peers == null) return
        clipPhase = ClipPhase.FINISHING
        FaceChangerFeature.run { _ ->
            val peer = peers.previewed()
            if (peer == null) {
                clipPhase = ClipPhase.IDLE
                notice = "The test clip could not be saved. Try again."
                return@run
            }
            if (hearing) hearing = runCatching { peer.setMonitor(false) }.getOrDefault(false)
            folder.mkdirs()
            clearFolder(folder)
            val target = File(folder, "test-${System.currentTimeMillis()}.part")
            val made = runCatching { peer.finishRecording(target) }.getOrNull()
            if (made == null) {
                target.delete()
                clipPhase = ClipPhase.IDLE
                notice = "The test clip could not be saved. Try again."
                return@run
            }
            val named = File(folder, target.nameWithoutExtension + clipExtension(made.mime))
            val file = if (target.renameTo(named)) named else target
            clip = TestClip(file, made.mime.startsWith("video/"), made.durationMs)
            playToken += 1
            clipPhase = ClipPhase.PLAYING
        }
    }

    fun record() {
        if (peers == null || phase != PreviewPhase.READY || calling) return
        if (clipPhase == ClipPhase.STARTING || clipPhase == ClipPhase.RECORDING || clipPhase == ClipPhase.FINISHING) return
        discard()
        notice = ""
        clipPhase = ClipPhase.STARTING
        FaceChangerFeature.run { current ->
            val peer = peers.previewed()
            val started = peer != null && runCatching {
                peer.startRecording(current.face, current.background, current.effectiveVoice)
            }.onFailure { notice = testClipFailure(it) }.isSuccess
            if (started) {
                remainingMs = TEST_CLIP_MS
                clipPhase = ClipPhase.RECORDING
            } else {
                clipPhase = ClipPhase.IDLE
                if (notice.isEmpty()) notice = "The test clip could not start."
            }
        }
    }

    fun call() {
        if (calling || phase == PreviewPhase.BUSY) return
        calling = true
        notice = ""
        clip = null
        clipPhase = ClipPhase.IDLE
        val conversation = precall.conversation
        val media = precall.media
        val wasHearing = hearing
        DirectCallFeature.run { engine ->
            if (wasHearing) {
                hearing = false
                runCatching { peers?.previewed()?.setMonitor(false) }
            }
            if (!engine.start(conversation, media)) {
                calling = false
                notice = "The call could not start. Try again in a moment."
            }
        }
    }

    if (peers != null) {
        LaunchedEffect(attempt) {
            phase = PreviewPhase.OPENING
            notice = ""
            FaceChangerFeature.run { current ->
                if (peers.busy || DirectCallFeature.engine?.active == true) {
                    phase = PreviewPhase.BUSY
                    return@run
                }
                val opened = runCatching {
                    peers.preview(
                        current.face,
                        current.background,
                        current.customBackgroundImage,
                        current.customFaceImage,
                        media = precall.media,
                        owner = DirectCallFeature.PRECALL_OWNER,
                    )
                }.getOrNull()
                if (opened == null) {
                    phase = PreviewPhase.FAILED
                    return@run
                }
                phase = PreviewPhase.READY
                if (callHeadphonesConnected(context)) hearing = runCatching { opened.setMonitor(true) }.getOrDefault(false)
            }
        }
        LaunchedEffect(previewing) {
            if (!previewing && !calling && phase == PreviewPhase.READY) {
                clipPhase = ClipPhase.IDLE
                attempt += 1
            }
        }
        DisposableEffect(peers) {
            onDispose {
                FaceChangerFeature.run { _ ->
                    clearFolder(folder)
                    if (DirectCallFeature.engine?.active != true) peers.endPreview(DirectCallFeature.PRECALL_OWNER)
                }
            }
        }
    }
    LaunchedEffect(clipPhase) {
        if (clipPhase != ClipPhase.RECORDING) return@LaunchedEffect
        val started = System.currentTimeMillis()
        while (true) {
            val left = TEST_CLIP_MS - (System.currentTimeMillis() - started)
            remainingMs = left.coerceAtLeast(0L)
            if (left <= 0L) break
            delay(200)
        }
        finish()
    }
    BackHandler { cancel() }

    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .clickable(indication = null, interactionSource = null) {},
    ) {
        if (webView != null && previewing) {
            key(webView) {
                AndroidView(factory = { host -> webView.adoptContext(host) }, modifier = Modifier.fillMaxSize())
            }
        }
        if (!video) {
            Column(
                Modifier.fillMaxSize().background(CALL_BACKDROP),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                CallAvatar(precall.partner)
                Spacer(Modifier.height(FmSpace.x3))
                Text(
                    "Voice: ${FaceChangerLook.voiceLabel(look.effectiveVoice)}",
                    color = Color.White.copy(alpha = .75f),
                    style = Fm.text.bodyMedium,
                )
            }
        }
        val reviewing = clip
        if (reviewing != null && (clipPhase == ClipPhase.PLAYING || clipPhase == ClipPhase.DONE)) {
            ClipReview(
                clip = reviewing,
                playToken = playToken,
                playing = clipPhase == ClipPhase.PLAYING,
                onDone = { if (clip === reviewing) clipPhase = ClipPhase.DONE },
                onFailed = {
                    if (clip === reviewing) {
                        clipPhase = ClipPhase.DONE
                        notice = "The test clip could not play."
                    }
                },
            )
        }
        when (phase) {
            PreviewPhase.OPENING -> Box(Modifier.align(Alignment.Center)) { FmCircularProgressIndicator() }
            PreviewPhase.FAILED, PreviewPhase.BUSY -> Column(
                Modifier.align(Alignment.Center).padding(FmSpace.x4),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    when {
                        phase == PreviewPhase.BUSY -> "Another call is using the camera and microphone."
                        video -> "The camera could not start. You can still call."
                        else -> "The microphone could not start. You can still call."
                    },
                    color = Color.White,
                    textAlign = TextAlign.Center,
                    style = Fm.text.bodyMedium,
                )
                if (peers != null) {
                    Spacer(Modifier.height(FmSpace.x3))
                    PillButton("Try again", background = colors.accent) { attempt += 1 }
                }
            }
            PreviewPhase.READY -> Unit
        }
        Column(
            Modifier
                .align(Alignment.TopCenter)
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(top = FmSpace.x4, start = FmSpace.x4, end = FmSpace.x4),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "Call @${precall.partner}",
                color = Color.White,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.titleLarge,
            )
            Spacer(Modifier.height(FmSpace.x1))
            Text(
                when (clipPhase) {
                    ClipPhase.STARTING -> "Getting ready to record…"
                    ClipPhase.RECORDING -> "Recording a test clip · ${(remainingMs + 999) / 1000} s left"
                    ClipPhase.FINISHING -> "Saving the test clip…"
                    ClipPhase.PLAYING -> "Playing your test clip"
                    ClipPhase.DONE -> "This is how you will look and sound"
                    ClipPhase.IDLE -> if (video) "Video call · only you can see this preview" else "Voice call · check how you sound"
                },
                color = Color.White.copy(alpha = .85f),
                textAlign = TextAlign.Center,
                style = Fm.text.bodyMedium,
            )
            Text(
                "Calls go directly between your devices when possible.",
                color = Color.White.copy(alpha = .7f),
                textAlign = TextAlign.Center,
                style = Fm.text.bodySmall,
            )
            if (notice.isNotEmpty()) {
                Text(
                    notice,
                    color = Color.White,
                    textAlign = TextAlign.Center,
                    style = Fm.text.bodySmall.copy(fontWeight = FontWeight.SemiBold),
                    modifier = Modifier
                        .padding(top = FmSpace.x2)
                        .clip(RoundedCornerShape(12.dp))
                        .background(Color.Black.copy(alpha = .55f))
                        .padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
                )
            }
            if (quality != null && previewing && phase == PreviewPhase.READY && clipPhase != ClipPhase.PLAYING && clipPhase != ClipPhase.DONE) {
                CallQualityWidget(quality, video = video, modifier = Modifier.align(Alignment.Start).padding(top = FmSpace.x2))
            }
        }
        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(bottom = FmSpace.x5, start = FmSpace.x4, end = FmSpace.x4)
                .fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            val ready = phase == PreviewPhase.READY && !calling
            if (ready && clipPhase == ClipPhase.IDLE) {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceEvenly,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FaceChangerButtons(
                        faces = video && faces,
                        thumbs = if (video) thumbs else null,
                        onProblem = { notice = it },
                    )
                    CallRoundButton(
                        icon = "hearing",
                        label = if (hearing) "Stop hearing myself" else "Hear myself",
                        background = if (hearing) Color.White else Color.White.copy(alpha = .18f),
                        tint = if (hearing) Color.Black else Color.White,
                    ) {
                        val next = !hearing
                        FaceChangerFeature.run { _ ->
                            hearing = runCatching { peers?.previewed()?.setMonitor(next) ?: false }.getOrDefault(false)
                        }
                    }
                }
                Spacer(Modifier.height(FmSpace.x4))
            }
            if (reviewing != null && (clipPhase == ClipPhase.PLAYING || clipPhase == ClipPhase.DONE)) {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(FmSpace.x2, Alignment.CenterHorizontally),
                ) {
                    PillButton("Replay", background = Color.White.copy(alpha = .18f)) {
                        playToken += 1
                        clipPhase = ClipPhase.PLAYING
                    }
                    PillButton("Re-record", background = Color.White.copy(alpha = .18f), enabled = ready) { record() }
                    PillButton("Discard", background = Color.White.copy(alpha = .18f)) { discard() }
                }
            } else {
                val recording = clipPhase == ClipPhase.RECORDING
                PillButton(
                    label = when (clipPhase) {
                        ClipPhase.RECORDING -> "Stop · ${(remainingMs + 999) / 1000} s"
                        ClipPhase.STARTING -> "Starting…"
                        ClipPhase.FINISHING -> "Saving…"
                        else -> "Record test clip"
                    },
                    background = if (recording) colors.danger else Color.White.copy(alpha = .18f),
                    enabled = ready && clipPhase != ClipPhase.STARTING && clipPhase != ClipPhase.FINISHING,
                ) {
                    if (recording) finish() else record()
                }
            }
            Spacer(Modifier.height(FmSpace.x4))
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                PillButton("Cancel", background = Color.White.copy(alpha = .18f), modifier = Modifier.weight(1f)) { cancel() }
                PillButton(
                    label = if (calling) "Calling…" else "Call",
                    background = colors.success,
                    icon = if (video) "videocam" else "call",
                    modifier = Modifier.weight(1f),
                    enabled = !calling && phase != PreviewPhase.BUSY,
                ) { call() }
            }
        }
    }
}

@Composable
private fun ClipReview(
    clip: TestClip,
    playToken: Int,
    playing: Boolean,
    onDone: () -> Unit,
    onFailed: () -> Unit,
) {
    val context = LocalContext.current
    DisposableEffect(clip.file.path, playToken) {
        val held = CallSpeaker.hold(context)
        onDispose { if (held) CallSpeaker.release(context) }
    }
    Box(Modifier.fillMaxSize().background(CALL_BACKDROP)) {
        if (clip.video) {
            key(clip.file.path, playToken) {
                AndroidView(
                    factory = { host ->
                        FrameLayout(host).apply {
                            setBackgroundColor(android.graphics.Color.BLACK)
                            val player = VideoView(host)
                            player.setAudioAttributes(CLIP_AUDIO)
                            player.setOnPreparedListener { player.start() }
                            player.setOnCompletionListener { onDone() }
                            player.setOnErrorListener { _, _, _ ->
                                onFailed()
                                true
                            }
                            player.setVideoPath(clip.file.absolutePath)
                            addView(
                                player,
                                FrameLayout.LayoutParams(
                                    ViewGroup.LayoutParams.WRAP_CONTENT,
                                    ViewGroup.LayoutParams.WRAP_CONTENT,
                                    Gravity.CENTER,
                                ),
                            )
                        }
                    },
                    onRelease = { holder -> (holder.getChildAt(0) as? VideoView)?.stopPlayback() },
                    modifier = Modifier.fillMaxSize().padding(top = 120.dp, bottom = 200.dp),
                )
            }
        } else {
            DisposableEffect(clip.file.path, playToken) {
                val player = runCatching {
                    MediaPlayer().apply {
                        setAudioAttributes(CLIP_AUDIO)
                        setDataSource(clip.file.absolutePath)
                        setOnCompletionListener { onDone() }
                        prepare()
                        start()
                    }
                }.getOrNull()
                if (player == null) onFailed()
                onDispose { player?.release() }
            }
            Column(
                Modifier.align(Alignment.Center),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Box(
                    Modifier.size(112.dp).clip(CircleShape).background(Fm.colors.accent.copy(alpha = .35f)),
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("graphic_eq", size = 52.dp, tint = Color.White)
                }
                Spacer(Modifier.height(FmSpace.x3))
                Text(
                    if (playing) "Listening to your test clip…" else "Test clip · ${(clip.durationMs + 500) / 1000} s",
                    color = Color.White.copy(alpha = .85f),
                    style = Fm.text.bodyMedium,
                )
            }
        }
    }
}
