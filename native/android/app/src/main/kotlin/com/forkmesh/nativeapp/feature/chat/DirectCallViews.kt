package com.forkmesh.nativeapp.feature.chat

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import androidx.activity.compose.BackHandler
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectCallInvite
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.DirectCallPhase
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmIconTile
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.launch

private data class DirectCallView(
    val phase: DirectCallPhase?,
    val invite: DirectCallInvite?,
    val media: DirectCallKind,
    val partner: String,
    val status: String,
    val muted: Boolean,
    val cameraOff: Boolean,
    val face: String,
    val available: Boolean,
)

private fun DirectCallEngine.view(): DirectCallView = DirectCallView(
    phase = phase,
    invite = invite,
    media = media,
    partner = partner,
    status = stateLine,
    muted = muted,
    cameraOff = cameraOff,
    face = face,
    available = available,
)

private fun callPermissions(kind: DirectCallKind): Array<String> =
    if (kind == DirectCallKind.VIDEO) {
        arrayOf(Manifest.permission.RECORD_AUDIO, Manifest.permission.CAMERA)
    } else {
        arrayOf(Manifest.permission.RECORD_AUDIO)
    }

private fun callPermitted(context: Context, kind: DirectCallKind): Boolean =
    callPermissions(kind).all { context.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

private fun deniedNotice(kind: DirectCallKind): String =
    if (kind == DirectCallKind.VIDEO) "Allow the camera and microphone to use video calls." else "Allow the microphone to use voice calls."

@Composable
private fun rememberCallPermissionGate(onGranted: (DirectCallKind) -> Unit): (DirectCallKind) -> Unit {
    val context = LocalContext.current
    val granted by rememberUpdatedState(onGranted)
    var wanted by remember { mutableStateOf<DirectCallKind?>(null) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { _ ->
        val kind = wanted ?: return@rememberLauncherForActivityResult
        wanted = null
        if (callPermitted(context, kind)) granted(kind) else ChatDepth.post(deniedNotice(kind))
    }
    return { kind ->
        if (callPermitted(context, kind)) {
            granted(kind)
        } else {
            wanted = kind
            launcher.launch(callPermissions(kind))
        }
    }
}

@Composable
private fun rememberCallView(): Pair<DirectCallEngine, DirectCallView>? {
    val revision by DirectCallFeature.revision.collectAsState()
    val engine = DirectCallFeature.engine ?: return null
    val view = remember(revision, engine) { engine.view() }
    return engine to view
}

@Composable
fun DirectCallButtons(conversation: String) {
    if (!DirectMessageFrames.isDirect(conversation)) return
    val (_, state) = rememberCallView() ?: return
    if (!state.available) return
    val start = rememberCallPermissionGate { kind ->
        DirectCallFeature.run { engine ->
            if (!engine.start(conversation, kind)) ChatDepth.post("The call could not start. Try again in a moment.")
        }
    }
    val idle = state.phase == null
    FmIconButton(icon = "call", label = "Voice call", onPressed = if (idle) ({ start(DirectCallKind.AUDIO) }) else null)
    FmIconButton(icon = "videocam", label = "Video call", onPressed = if (idle) ({ start(DirectCallKind.VIDEO) }) else null)
}

@Composable
fun DirectCallOverlay() {
    val (engine, state) = rememberCallView() ?: return
    when {
        state.phase != null -> DirectCallScreen(state)
        state.invite != null -> IncomingCallBanner(engine, state.invite)
    }
}

@Composable
private fun IncomingCallBanner(engine: DirectCallEngine, invite: DirectCallInvite) {
    val colors = Fm.colors
    val accept = rememberCallPermissionGate { _ -> DirectCallFeature.run { if (it === engine) it.acceptInvite() } }
    Box(Modifier.fillMaxSize().statusBarsPadding().padding(FmSpace.x3), contentAlignment = Alignment.TopCenter) {
        Row(
            Modifier
                .fillMaxWidth()
                .shadow(12.dp, RoundedCornerShape(FmRadius.lg))
                .clip(RoundedCornerShape(FmRadius.lg))
                .background(colors.bgElevated)
                .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIconTile(icon = if (invite.media == DirectCallKind.VIDEO) "videocam" else "call", tint = colors.success)
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f)) {
                Text(
                    "@${invite.partner}",
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.titleMedium.copy(color = colors.textPrimary),
                )
                Text(
                    if (invite.media == DirectCallKind.VIDEO) "Incoming video call" else "Incoming voice call",
                    style = Fm.text.bodySmall.copy(color = colors.textTertiary),
                )
            }
            CallRoundButton(icon = "call_end", label = "Decline", background = colors.danger, size = 44) {
                DirectCallFeature.run { it.declineInvite() }
            }
            Spacer(Modifier.width(FmSpace.x3))
            CallRoundButton(icon = "call", label = "Accept", background = colors.success, size = 44) { accept(invite.media) }
        }
    }
}

@Composable
private fun DirectCallScreen(state: DirectCallView) {
    val colors = Fm.colors
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val hostView = LocalView.current
    val peers = DirectCallFeature.peers
    val webView = peers?.view?.collectAsState()?.value
    val facesSupported = peers?.facesSupported?.collectAsState()?.value == true
    val video = state.media == DirectCallKind.VIDEO
    DisposableEffect(hostView) {
        hostView.keepScreenOn = true
        onDispose { hostView.keepScreenOn = false }
    }
    BackHandler {}
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .clickable(indication = null, interactionSource = null) {},
    ) {
        if (webView != null) {
            key(webView) {
                AndroidView(factory = { context -> webView.adoptContext(context) }, modifier = Modifier.fillMaxSize())
            }
        }
        if (!video || state.phase != DirectCallPhase.LIVE) {
            Column(Modifier.align(Alignment.Center), horizontalAlignment = Alignment.CenterHorizontally) {
                Box(
                    Modifier.size(112.dp).clip(CircleShape).background(colors.accent.copy(alpha = .35f)),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        state.partner.take(1).uppercase().ifEmpty { "?" },
                        color = Color.White,
                        fontSize = 44.sp,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
        }
        Column(
            Modifier.align(Alignment.TopCenter).statusBarsPadding().padding(top = FmSpace.x6, start = FmSpace.x4, end = FmSpace.x4),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "@${state.partner}",
                color = Color.White,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.titleLarge,
            )
            Spacer(Modifier.height(FmSpace.x1))
            Text(state.status, color = Color.White.copy(alpha = .75f), style = Fm.text.bodyMedium)
        }
        Row(
            Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(bottom = FmSpace.x6, start = FmSpace.x4, end = FmSpace.x4)
                .fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            CallRoundButton(
                icon = if (state.muted) "mic_off" else "mic",
                label = if (state.muted) "Unmute" else "Mute",
                background = if (state.muted) Color.White else Color.White.copy(alpha = .18f),
                tint = if (state.muted) Color.Black else Color.White,
            ) { DirectCallFeature.run { it.toggleMute() } }
            if (video) {
                CallRoundButton(
                    icon = if (state.cameraOff) "videocam_off" else "videocam",
                    label = if (state.cameraOff) "Camera on" else "Camera off",
                    background = if (state.cameraOff) Color.White else Color.White.copy(alpha = .18f),
                    tint = if (state.cameraOff) Color.Black else Color.White,
                ) { DirectCallFeature.run { it.toggleCamera() } }
                if (facesSupported) {
                    CallRoundButton(
                        icon = if (state.face.isEmpty()) "face" else "theater_comedy",
                        label = "Face",
                        background = if (state.face.isEmpty()) Color.White.copy(alpha = .18f) else colors.accent,
                    ) {
                        scope.launch {
                            val choice = pickCallFace(messenger, state.face) ?: return@launch
                            DirectCallFeature.run { it.setFace(choice) }
                        }
                    }
                }
            }
            CallRoundButton(icon = "call_end", label = "Hang up", background = colors.danger) {
                DirectCallFeature.run { it.hangUp() }
            }
        }
    }
}

private suspend fun pickCallFace(messenger: FmMessenger, current: String): String? =
    messenger.showModalBottomSheet<String> { close ->
        Column(Modifier.padding(bottom = FmSpace.x4)) {
            Text(
                "Face",
                modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary),
            )
            val options = listOf("" to "Your own face") + DirectCallEngine.FACES.map { it.id to it.label }
            options.forEachIndexed { index, (id, label) ->
                if (index > 0) FmRowDivider()
                FmListRow(
                    title = label,
                    leading = { FmIconTile(icon = if (id.isEmpty()) "face" else "theater_comedy") },
                    trailing = if (id == current) ({ FmIcon("check", size = 20.dp, tint = Fm.colors.accent) }) else null,
                    showChevron = false,
                    onTap = { close(id) },
                )
            }
        }
    }

@Composable
private fun CallRoundButton(
    icon: String,
    label: String,
    background: Color,
    tint: Color = Color.White,
    size: Int = 60,
    onPressed: () -> Unit,
) {
    Box(
        Modifier
            .size(size.dp)
            .clip(CircleShape)
            .background(background)
            .clickable(onClick = onPressed)
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) {
        FmIcon(icon, size = (size * 0.45f).dp, tint = tint)
    }
}
