package com.forkmesh.nativeapp.feature.facechanger

import android.Manifest
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.io.File
import kotlinx.coroutines.delay

private val MEDIA_PERMISSIONS = arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)

private fun mediaPermitted(context: Context): Boolean =
    MEDIA_PERMISSIONS.all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

private enum class StudioPhase { OPENING, READY, FAILED, BUSY, STARTING, RECORDING, FINISHING }

private const val CLIP_LIMIT_MS = 5 * 60 * 1000L
private const val SHARE_DIR = "face-changer-share"

class FaceChangerScreen : Screen() {
    override val title: String get() = "Face Changer"

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        var asked by remember { mutableStateOf(mediaPermitted(context)) }
        var permitted by remember { mutableStateOf(mediaPermitted(context)) }
        val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { _ ->
            asked = true
            permitted = mediaPermitted(context)
        }
        LaunchedEffect(Unit) { if (!permitted) ask.launch(MEDIA_PERMISSIONS) }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Face Changer", navigator)
            when {
                !asked -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                !permitted -> Column(
                    Modifier.fillMaxSize().padding(FmSpace.x4),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    FmEmptyState(
                        "face",
                        "Allow the camera and microphone",
                        Modifier.weight(1f, fill = false),
                        message = "Face Changer needs them to change your face and voice.",
                    )
                    FmToneButton("Allow", onPressed = { ask.launch(MEDIA_PERMISSIONS) }, tone = FmTone.Accent)
                }
                else -> FaceChangerStudio(Modifier.weight(1f).fillMaxWidth())
            }
        }
    }
}

@Composable
private fun FaceChangerStudio(modifier: Modifier) {
    val context = LocalContext.current
    val peers = FaceChangerFeature.peers
    if (peers == null) {
        FmEmptyState("face", "Face Changer is not available", modifier, message = "Restart the app to open it.")
        return
    }
    val webView by peers.view.collectAsState()
    val previewing by peers.previewing.collectAsState()
    val faces by peers.facesSupported.collectAsState()
    val thumbs by peers.backgrounds.collectAsState()
    var phase by remember { mutableStateOf(StudioPhase.OPENING) }
    var notice by remember { mutableStateOf("") }
    var hearing by remember { mutableStateOf(false) }
    var startedAt by remember { mutableLongStateOf(0L) }
    var attempt by remember { mutableIntStateOf(0) }

    fun finish() {
        if (phase != StudioPhase.RECORDING) return
        phase = StudioPhase.FINISHING
        FaceChangerFeature.run { _ ->
            val peer = peers.previewed()
            if (peer == null) {
                phase = StudioPhase.FAILED
                return@run
            }
            if (hearing) hearing = runCatching { peer.setMonitor(false) }.getOrDefault(false)
            val folder = File(context.cacheDir, SHARE_DIR).apply { mkdirs() }
            folder.listFiles()?.forEach { if (it.isFile) it.delete() }
            val target = File(folder, "face-changer-${System.currentTimeMillis()}.mp4")
            val result = runCatching { peer.finishRecording(target) }
            val made = result.getOrNull()
            phase = StudioPhase.READY
            if (made == null) {
                target.delete()
                notice = saveFailure(result.exceptionOrNull())
                return@run
            }
            val file = if (made.mime == "video/mp4") target else File(folder, target.nameWithoutExtension + ".webm").also { target.renameTo(it) }
            ContextCompat.getMainExecutor(context).execute { shareClip(context, file, made.mime)?.let { notice = it } }
        }
    }

    LaunchedEffect(attempt) {
        phase = StudioPhase.OPENING
        notice = ""
        FaceChangerFeature.run { look ->
            if (peers.busy) {
                phase = StudioPhase.BUSY
                return@run
            }
            val opened = runCatching {
                peers.preview(look.face, look.background, look.customBackgroundImage, look.customFaceImage)
            }.getOrNull()
            if (opened == null) {
                phase = StudioPhase.FAILED
                return@run
            }
            phase = StudioPhase.READY
            if (callHeadphonesConnected(context)) hearing = runCatching { opened.setMonitor(true) }.getOrDefault(false)
        }
    }
    LaunchedEffect(previewing) {
        if (!previewing && (phase == StudioPhase.READY || phase == StudioPhase.RECORDING)) attempt += 1
    }
    LaunchedEffect(phase, startedAt) {
        if (phase != StudioPhase.RECORDING) return@LaunchedEffect
        delay(CLIP_LIMIT_MS)
        finish()
    }
    DisposableEffect(peers) {
        onDispose { FaceChangerFeature.run { _ -> peers.endPreview() } }
    }

    Box(modifier.background(Color.Black)) {
        val view = webView
        if (view != null && previewing) {
            key(view) {
                AndroidView(factory = { host -> view.adoptContext(host) }, modifier = Modifier.fillMaxSize())
            }
        }
        when (phase) {
            StudioPhase.OPENING -> Box(Modifier.align(Alignment.Center)) { FmCircularProgressIndicator() }
            StudioPhase.FAILED, StudioPhase.BUSY -> Column(
                Modifier.align(Alignment.Center).padding(FmSpace.x4),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    if (phase == StudioPhase.BUSY) {
                        "A call is using the camera. Change your face from the call screen, or come back when it ends."
                    } else {
                        "The camera could not start."
                    },
                    color = Color.White,
                    textAlign = TextAlign.Center,
                    style = Fm.text.bodyMedium,
                )
                Spacer(Modifier.height(FmSpace.x3))
                FmToneButton("Try again", onPressed = { attempt += 1 }, tone = FmTone.Accent)
            }
            else -> Unit
        }
        Column(
            Modifier.align(Alignment.TopCenter).padding(top = FmSpace.x4, start = FmSpace.x4, end = FmSpace.x4),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                when (phase) {
                    StudioPhase.RECORDING -> "Recording · stops by itself after 5 minutes"
                    StudioPhase.STARTING -> "Getting the face, background and voice ready…"
                    StudioPhase.FINISHING -> "Saving the clip…"
                    else -> "Only you can see this. Calls use the face, background and voice you pick here."
                },
                color = Color.White.copy(alpha = .85f),
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
        }
        val live = phase == StudioPhase.READY || phase == StudioPhase.RECORDING
        Column(
            Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(bottom = FmSpace.x5, start = FmSpace.x4, end = FmSpace.x4)
                .fillMaxWidth(),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            if (phase == StudioPhase.READY) {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceEvenly,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    FaceChangerButtons(faces = faces, thumbs = thumbs, onProblem = { notice = it })
                    CallRoundButton(
                        icon = "hearing",
                        label = if (hearing) "Stop hearing myself" else "Hear myself",
                        background = if (hearing) Color.White else Color.White.copy(alpha = .18f),
                        tint = if (hearing) Color.Black else Color.White,
                    ) {
                        val next = !hearing
                        FaceChangerFeature.run { _ ->
                            hearing = runCatching { peers.previewed()?.setMonitor(next) ?: false }.getOrDefault(false)
                        }
                    }
                }
                Spacer(Modifier.height(FmSpace.x4))
            }
            Box(
                Modifier
                    .height(50.dp)
                    .clip(RoundedCornerShape(25.dp))
                    .background(if (live) Fm.colors.danger else Color.White.copy(alpha = .18f))
                    .clickable(enabled = live) {
                        if (phase == StudioPhase.RECORDING) {
                            finish()
                            return@clickable
                        }
                        notice = ""
                        phase = StudioPhase.STARTING
                        FaceChangerFeature.run { look ->
                            val peer = peers.previewed()
                            val started = peer != null && runCatching {
                                peer.startRecording(look.face, look.background, look.effectiveVoice)
                            }.onFailure { notice = recordFailure(it) }.isSuccess
                            if (started) {
                                startedAt = System.currentTimeMillis()
                                phase = StudioPhase.RECORDING
                            } else {
                                phase = StudioPhase.READY
                                if (notice.isEmpty()) notice = "Recording could not start."
                            }
                        }
                    }
                    .padding(horizontal = 22.dp)
                    .semantics { contentDescription = if (phase == StudioPhase.RECORDING) "Stop and share the clip" else "Record a clip" },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    if (phase == StudioPhase.RECORDING) "Stop and share" else "Record a clip",
                    color = Color.White,
                    fontWeight = FontWeight.SemiBold,
                )
            }
        }
    }
}

private fun saveFailure(error: Throwable?): String =
    if (generateSequence(error) { it.cause }.any { it.message == "empty_recording" }) {
        "Nothing was recorded. Record for a little longer, then stop."
    } else {
        "The clip could not be saved. Try again."
    }

private fun recordFailure(error: Throwable): String = when (error.message) {
    "face_not_ready", "face_unavailable", "background_unavailable" -> "The face or background could not start here, so nothing was recorded."
    "face_not_found" -> "No face was found in your photo, so nothing was recorded."
    "voice_unavailable" -> "The changed voice could not start here, so nothing was recorded."
    "recording_unsupported" -> "This phone cannot record from the Face Changer."
    else -> "Recording could not start."
}

private fun shareClip(context: Context, file: File, mime: String): String? {
    val started = runCatching {
        val uri = FileProvider.getUriForFile(context, PhotoPicker.authority(context), file)
        val send = Intent(Intent.ACTION_SEND)
            .setType(mime)
            .putExtra(Intent.EXTRA_STREAM, uri)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        send.clipData = ClipData.newRawUri(file.name, uri)
        context.startActivity(
            Intent.createChooser(send, "Share your clip"),
        )
    }
    return if (started.isFailure) "No app on this phone can take the clip." else null
}
