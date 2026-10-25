package com.forkmesh.nativeapp.feature.chat

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.GroupCallView
import com.forkmesh.nativeapp.feature.facechanger.CallRoundButton
import com.forkmesh.nativeapp.feature.facechanger.adoptContext
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.math.roundToInt

private val controlFill = Color(0xFF404040)

private fun cssColor(color: Color, alpha: Float): String =
    "rgba(${(color.red * 255).roundToInt()},${(color.green * 255).roundToInt()},${(color.blue * 255).roundToInt()},$alpha)"

@Composable
fun GroupCallButtons(conversation: String) {
    val revision by GroupCallFeature.revision.collectAsState()
    val call by GroupCallFeature.view.collectAsState()
    if (call.active || !GroupCallFeature.canCall(conversation)) return
    val count = remember(revision, conversation) { GroupCallFeature.inCall(conversation) }
    val accent = cssColor(Fm.colors.accent, .35f)
    val start = rememberCallPermissionGate { kind ->
        GroupCallFeature.join(conversation, kind, accent)?.let(ChatDepth::post)
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        if (count > 0) {
            Row(
                Modifier
                    .clip(CircleShape)
                    .background(Fm.colors.success)
                    .clickable(role = Role.Button) { start(DirectCallKind.AUDIO) }
                    .semantics(mergeDescendants = true) { contentDescription = "Join the call in progress, $count in call" }
                    .padding(horizontal = 10.dp, vertical = 5.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                FmIcon("call", size = 14.dp, tint = Color.White)
                Text("Join · $count", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
        }
        FmIconButton(icon = "videocam", label = "Video group call", onPressed = { start(DirectCallKind.VIDEO) })
        FmIconButton(icon = "call", label = "Voice group call", onPressed = { start(DirectCallKind.AUDIO) })
    }
}

@Composable
fun GroupCallOverlay() {
    val call by GroupCallFeature.view.collectAsState()
    if (!call.active) return
    GroupCallPanel(call)
}

@Composable
private fun GroupCallPanel(call: GroupCallView) {
    val webView by GroupCallFeature.web.collectAsState()
    val quality by GroupCallFeature.quality.collectAsState()
    val hostView = LocalView.current
    val video = call.media == DirectCallKind.VIDEO
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
        Column(
            Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding(),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        ) {
            Column(
                Modifier.padding(top = FmSpace.x4, start = FmSpace.x4, end = FmSpace.x4),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                Text(call.title, color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(call.stateLine, color = Color.White.copy(alpha = .8f), fontSize = 13.sp)
            }
            CallQualityWidget(quality, video = video, peerLabel = "Others")
            Box(Modifier.weight(1f).fillMaxWidth()) {
                val page = webView
                if (page != null) {
                    key(page) {
                        AndroidView(factory = { context -> page.adoptContext(context) }, modifier = Modifier.fillMaxSize())
                    }
                }
            }
            Text(
                GroupCallView.NOTE,
                color = Color.White.copy(alpha = .6f),
                fontSize = 11.sp,
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(horizontal = FmSpace.x5),
            )
            Row(
                Modifier.padding(bottom = FmSpace.x5),
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x5),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CallRoundButton(
                    icon = if (call.muted) "mic_off" else "mic",
                    label = if (call.muted) "Unmute" else "Mute",
                    background = controlFill,
                ) { GroupCallFeature.toggleMute() }
                if (video) {
                    CallRoundButton(
                        icon = if (call.cameraOff) "videocam_off" else "videocam",
                        label = if (call.cameraOff) "Camera on" else "Camera off",
                        background = controlFill,
                    ) { GroupCallFeature.toggleCamera() }
                }
                CallRoundButton(icon = "call_end", label = "Leave", background = Fm.colors.danger) { GroupCallFeature.leave() }
            }
        }
    }
}
