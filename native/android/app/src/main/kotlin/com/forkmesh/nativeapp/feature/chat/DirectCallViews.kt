package com.forkmesh.nativeapp.feature.chat

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.os.Build
import android.view.WindowManager
import android.webkit.WebView
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.offset
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.layout.boundsInParent
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.foundation.border
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import com.forkmesh.nativeapp.chat.CallMiniLayout
import com.forkmesh.nativeapp.chat.CallMiniSide
import com.forkmesh.nativeapp.chat.CallMiniSpot
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.delay
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.CallControlsArrangement
import com.forkmesh.nativeapp.chat.CallControlsLayout
import com.forkmesh.nativeapp.chat.CallPreviewCorner
import com.forkmesh.nativeapp.chat.CallPreviewLayout
import com.forkmesh.nativeapp.core.Services
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectCallInvite
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.DirectCallPhase
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.facechanger.FaceChangerLook
import com.forkmesh.nativeapp.feature.facechanger.CallRoundButton
import com.forkmesh.nativeapp.feature.facechanger.FaceChangerButtons
import com.forkmesh.nativeapp.feature.facechanger.adoptContext
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace

private data class DirectCallView(
    val phase: DirectCallPhase?,
    val invite: DirectCallInvite?,
    val media: DirectCallKind,
    val partner: String,
    val status: String,
    val muted: Boolean,
    val cameraOff: Boolean,
    val available: Boolean,
    val voice: String = "",
    val reconnecting: Boolean = false,
    val liveSince: Long? = null,
)

private fun DirectCallEngine.view(): DirectCallView = DirectCallView(
    phase = phase,
    invite = invite,
    media = media,
    partner = partner,
    status = stateLine,
    muted = muted,
    cameraOff = cameraOff,
    available = available,
    voice = voice,
    reconnecting = reconnecting,
    liveSince = liveSince,
)

internal fun callPermissions(kind: DirectCallKind): Array<String> =
    if (kind == DirectCallKind.VIDEO) {
        arrayOf(Manifest.permission.RECORD_AUDIO, Manifest.permission.CAMERA)
    } else {
        arrayOf(Manifest.permission.RECORD_AUDIO)
    }

private fun callPermitted(context: Context, kind: DirectCallKind): Boolean = DirectCallFeature.permitted(context, kind)

private fun deniedNotice(kind: DirectCallKind): String =
    if (kind == DirectCallKind.VIDEO) "Allow the camera and microphone to use video calls." else "Allow the microphone to use voice calls."

@Composable
internal fun rememberCallPermissionGate(onGranted: (DirectCallKind) -> Unit): (DirectCallKind) -> Unit {
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
        DirectCallFeature.openPreCall(conversation, kind)?.let(ChatDepth::post)
    }
    val idle = state.phase == null
    FmIconButton(icon = "call", label = "Voice call", onPressed = if (idle) ({ start(DirectCallKind.AUDIO) }) else null)
    FmIconButton(icon = "videocam", label = "Video call", onPressed = if (idle) ({ start(DirectCallKind.VIDEO) }) else null)
}

@Composable
private fun rememberAnswerGate(): (DirectCallInvite) -> Unit {
    val context = LocalContext.current
    var wanted by remember { mutableStateOf<DirectCallInvite?>(null) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { _ ->
        val invite = wanted ?: return@rememberLauncherForActivityResult
        wanted = null
        DirectCallFeature.answer(invite.callId, invite.media, callPermitted(context, invite.media))
    }
    return { invite ->
        if (callPermitted(context, invite.media)) {
            DirectCallFeature.answer(invite.callId, invite.media, true)
        } else {
            DirectCallFeature.run { if (it.invite?.callId == invite.callId) it.holdInvite() }
            wanted = invite
            launcher.launch(callPermissions(invite.media))
        }
    }
}

@Composable
fun DirectCallOverlay() {
    CallWindowEffect()
    val (_, state) = rememberCallView() ?: return
    val precall by DirectCallFeature.precall.collectAsState()
    val minimized by DirectCallFeature.minimized.collectAsState()
    val answer = rememberAnswerGate()
    val pending = precall
    if (state.phase == null && pending != null) PreCallScreen(pending)
    when {
        state.phase != null && minimized -> DirectCallMiniWindow(state)
        state.phase != null -> DirectCallScreen(state)
        state.invite != null -> key(state.invite.callId) { IncomingCallScreen(state.invite, answer) }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

private fun Activity.applyCallWindow(state: CallWindow) {
    if (Build.VERSION.SDK_INT >= 27) {
        setShowWhenLocked(state.showWhenLocked)
        setTurnScreenOn(state.turnScreenOn)
    } else {
        @Suppress("DEPRECATION")
        val locked = WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED
        @Suppress("DEPRECATION")
        val wake = WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON
        if (state.showWhenLocked) window.addFlags(locked) else window.clearFlags(locked)
        if (state.turnScreenOn) window.addFlags(wake) else window.clearFlags(wake)
    }
}

@Composable
private fun CallWindowEffect() {
    val wanted by CallRinger.window.collectAsState()
    val context = LocalContext.current
    DisposableEffect(wanted, context) {
        val activity = context.findActivity()
        activity?.applyCallWindow(wanted)
        onDispose { activity?.applyCallWindow(CallWindow()) }
    }
}

@Composable
internal fun CallAvatar(name: String, size: Int = 112) {
    Box(
        Modifier.size(size.dp).clip(CircleShape).background(Fm.colors.accent.copy(alpha = .35f)),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            name.take(1).uppercase().ifEmpty { "?" },
            color = Color.White,
            fontSize = (size * 0.4f).sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

@Composable
private fun LabeledCallButton(icon: String, label: String, background: Color, onPressed: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        CallRoundButton(icon = icon, label = label, background = background, size = 76, onPressed = onPressed)
        Spacer(Modifier.height(FmSpace.x2))
        Text(label, color = Color.White, style = Fm.text.bodyMedium)
    }
}

@Composable
private fun IncomingCallScreen(invite: DirectCallInvite, onAccept: (DirectCallInvite) -> Unit) {
    val colors = Fm.colors
    val video = invite.media == DirectCallKind.VIDEO
    BackHandler {}
    Box(
        Modifier
            .fillMaxSize()
            .background(Color(0xFF0E1116))
            .clickable(indication = null, interactionSource = null) {},
    ) {
        Column(
            Modifier.align(Alignment.TopCenter).statusBarsPadding().padding(top = 72.dp, start = FmSpace.x4, end = FmSpace.x4),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(if (video) "Video call" else "Voice call", color = Color.White.copy(alpha = .75f), style = Fm.text.bodyLarge)
            Spacer(Modifier.height(FmSpace.x5))
            CallAvatar(invite.partner, size = 128)
            Spacer(Modifier.height(FmSpace.x4))
            Text(
                "@${invite.partner}",
                color = Color.White,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.headlineSmall,
            )
            Spacer(Modifier.height(FmSpace.x1))
            Text("is calling you on ForkMesh", color = Color.White.copy(alpha = .75f), style = Fm.text.bodyMedium)
        }
        Row(
            Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(bottom = 56.dp, start = FmSpace.x4, end = FmSpace.x4)
                .fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            LabeledCallButton(icon = "call_end", label = "Decline", background = colors.danger) {
                DirectCallFeature.run { if (it.invite?.callId == invite.callId) it.declineInvite() }
            }
            LabeledCallButton(icon = if (video) "videocam" else "call", label = "Accept", background = colors.success) {
                onAccept(invite)
            }
        }
    }
}

@Composable
private fun DirectCallScreen(state: DirectCallView) {
    val colors = Fm.colors
    val hostView = LocalView.current
    val peers = DirectCallFeature.peers
    val webView = peers?.view?.collectAsState()?.value
    val facesSupported = peers?.facesSupported?.collectAsState()?.value == true
    val thumbs = peers?.backgrounds?.collectAsState()?.value
    val route = peers?.route?.collectAsState()?.value.orEmpty()
    val quality = peers?.quality?.collectAsState()?.value
    val remoteVideo = peers?.remoteVideo?.collectAsState()?.value == true
    val micLost = peers?.micLost?.collectAsState()?.value == true
    val video = state.media == DirectCallKind.VIDEO
    var headerBottom by remember { mutableStateOf(0f) }
    var controlsTop by remember { mutableStateOf(0f) }
    DisposableEffect(hostView) {
        hostView.keepScreenOn = true
        onDispose { hostView.keepScreenOn = false }
    }
    BackHandler { DirectCallFeature.minimize() }
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
        if (!video || !remoteVideo) {
            Column(
                Modifier
                    .align(Alignment.Center)
                    .semantics(mergeDescendants = true) {},
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                CallAvatar(state.partner, size = 110)
                Spacer(Modifier.height(FmSpace.x3))
                Text(
                    "@${state.partner}",
                    color = Color.White,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.headlineSmall.copy(fontWeight = FontWeight.SemiBold, shadow = CALL_TEXT_SHADOW),
                    modifier = Modifier.padding(horizontal = FmSpace.x4),
                )
            }
        }
        Column(
            Modifier
                .align(Alignment.TopCenter)
                .fillMaxWidth()
                .statusBarsPadding()
                .padding(top = FmSpace.x6, start = FmSpace.x4, end = FmSpace.x4)
                .onGloballyPositioned { headerBottom = it.boundsInParent().bottom },
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "@${state.partner}",
                color = Color.White,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.titleLarge.copy(shadow = CALL_TEXT_SHADOW),
                modifier = Modifier.padding(horizontal = FmSpace.x8),
            )
            Spacer(Modifier.height(FmSpace.x1))
            Text(
                state.status,
                color = Color.White.copy(alpha = .75f),
                style = Fm.text.bodyMedium.copy(shadow = CALL_TEXT_SHADOW),
            )
            if (state.phase == DirectCallPhase.LIVE && route.isNotEmpty()) {
                Text(
                    if (route == "relay") "Relayed · through a TURN relay server (third party)" else "Direct · peer-to-peer, no server in between",
                    color = Color.White.copy(alpha = .75f),
                    textAlign = TextAlign.Center,
                    style = Fm.text.bodySmall,
                )
            }
            if (state.voice.isNotEmpty()) {
                Text(
                    "Voice: ${FaceChangerLook.voiceLabel(state.voice)}",
                    color = Color.White.copy(alpha = .75f),
                    style = Fm.text.bodySmall,
                )
            }
            if (quality != null) {
                CallQualityWidget(
                    quality,
                    video = video,
                    sending = !state.cameraOff,
                    finishOnLeave = false,
                    modifier = Modifier.align(Alignment.Start).padding(top = FmSpace.x2),
                )
            }
            if (micLost) {
                Text(
                    "Your microphone stopped and could not restart. Check that no other app is using it.",
                    color = Color.White,
                    textAlign = TextAlign.Center,
                    style = Fm.text.bodySmall.copy(fontWeight = FontWeight.SemiBold),
                    modifier = Modifier
                        .padding(top = FmSpace.x2)
                        .clip(RoundedCornerShape(50))
                        .background(Color.Black.copy(alpha = .55f))
                        .padding(horizontal = FmSpace.x3, vertical = 6.dp),
                )
            }
        }
        Box(
            Modifier
                .align(Alignment.TopStart)
                .statusBarsPadding()
                .padding(start = FmSpace.x3, top = FmSpace.x2)
                .size(44.dp)
                .clip(CircleShape)
                .background(Color.Black.copy(alpha = .35f))
                .clickable(role = Role.Button, onClickLabel = "Keep the call going in a small window") {
                    DirectCallFeature.minimize()
                }
                .semantics { contentDescription = "Back to chat" },
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("keyboard_arrow_down", size = 26.dp, tint = Color.White)
        }
        if (webView != null && video && state.phase == DirectCallPhase.LIVE && !state.cameraOff && remoteVideo) {
            DraggableSelfView(webView, topClearance = headerBottom, bottomClearance = controlsTop)
        }
        DirectCallControls(
            state = state,
            looks = (if (video && facesSupported) 1 else 0) + (if (video && thumbs != null) 1 else 0) + 1,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .navigationBarsPadding()
                .padding(bottom = FmSpace.x6, start = FmSpace.x3, end = FmSpace.x3)
                .fillMaxWidth()
                .onGloballyPositioned { controlsTop = it.boundsInParent().top },
            looksContent = {
                FaceChangerButtons(
                    faces = video && facesSupported,
                    thumbs = if (video) thumbs else null,
                    voiceShown = state.voice,
                    onProblem = ChatDepth::post,
                )
            },
        )
    }
}


@Composable
private fun DirectCallControls(
    state: DirectCallView,
    looks: Int,
    modifier: Modifier,
    looksContent: @Composable () -> Unit,
) {
    val colors = Fm.colors
    val video = state.media == DirectCallKind.VIDEO
    val essentials: @Composable () -> Unit = {
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
        }
        CallRoundButton(icon = "call_end", label = "Hang up", size = CALL_HANG_UP_SIZE, background = colors.danger) {
            DirectCallFeature.run { it.hangUp() }
        }
    }
    val buttons = List(looks + if (video) 2 else 1) { CALL_BUTTON_SIZE.toFloat() } + CALL_HANG_UP_SIZE.toFloat()
    BoxWithConstraints(modifier, contentAlignment = Alignment.Center) {
        when (val arrangement = CallControlsLayout.arrange(maxWidth.value, buttons)) {
            is CallControlsArrangement.OneRow -> Row(
                horizontalArrangement = Arrangement.spacedBy(arrangement.spacing.dp, Alignment.CenterHorizontally),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                looksContent()
                essentials()
            }
            CallControlsArrangement.TwoRows -> Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(FmSpace.x4),
            ) {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(CallControlsLayout.LOOKS_ROW_GAP.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) { looksContent() }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(CallControlsLayout.ESSENTIALS_ROW_GAP.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) { essentials() }
            }
        }
    }
}

private const val CALL_BUTTON_SIZE = 60
private const val CALL_HANG_UP_SIZE = 64
private val CALL_TEXT_SHADOW = Shadow(Color.Black.copy(alpha = .6f), blurRadius = 12f)
private val MINI_SHAPE = RoundedCornerShape(14.dp)

@Composable
private fun DirectCallMiniWindow(state: DirectCallView) {
    val density = LocalDensity.current
    val prefs = remember { Services.get<PrefStore>() }
    val peers = DirectCallFeature.peers
    val webView = peers?.view?.collectAsState()?.value
    val remoteVideo = peers?.remoteVideo?.collectAsState()?.value == true
    val video = state.media == DirectCallKind.VIDEO
    val tileWidth = if (video) CallMiniLayout.VIDEO_WIDTH else CallMiniLayout.VOICE_WIDTH
    val tileHeight = if (video) CallMiniLayout.VIDEO_HEIGHT else CallMiniLayout.VOICE_HEIGHT
    var spot by remember {
        mutableStateOf(
            CallMiniSpot(
                CallMiniSide.from(prefs?.getString(CallMiniSide.SIDE_KEY)),
                prefs?.getDouble(CallMiniSide.Y_KEY) ?: CallMiniLayout.DEFAULT_FRACTION,
            ),
        )
    }
    var drag by remember { mutableStateOf(Offset.Zero) }
    var dragging by remember { mutableStateOf(false) }
    val status = rememberMiniStatus(state)
    BoxWithConstraints(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
        val width = maxWidth.value
        val height = maxHeight.value
        val center = CallMiniLayout.center(spot, tileWidth, tileHeight, width, height)
        val scale = if (dragging) 1.04f else 1f
        val left = center.x + drag.x - tileWidth * scale / 2
        val upper = center.y + drag.y - tileHeight * scale / 2
        val tracker = remember { VelocityTracker() }
        Box(
            Modifier
                .offset(left.dp, upper.dp)
                .size((tileWidth * scale).dp, (tileHeight * scale).dp)
                .shadow(10.dp, MINI_SHAPE)
                .clip(MINI_SHAPE)
                .background(Color.Black)
                .border(1.dp, Color.White.copy(alpha = .3f), MINI_SHAPE)
                .pointerInput(width, height, tileWidth, tileHeight) {
                    var total = Offset.Zero
                    detectDragGestures(
                        onDragStart = {
                            tracker.resetTracking()
                            total = Offset.Zero
                            dragging = true
                        },
                        onDragEnd = {
                            val velocity = tracker.calculateVelocity()
                            val next = CallMiniLayout.settle(
                                spot,
                                drag.x,
                                drag.y,
                                velocity.x / density.density,
                                velocity.y / density.density,
                                tileWidth,
                                tileHeight,
                                width,
                                height,
                            )
                            spot = next
                            prefs?.putString(CallMiniSide.SIDE_KEY, next.side.wire)
                            prefs?.putDouble(CallMiniSide.Y_KEY, next.fraction)
                            drag = Offset.Zero
                            dragging = false
                        },
                        onDragCancel = {
                            drag = Offset.Zero
                            dragging = false
                        },
                    ) { change, amount ->
                        change.consume()
                        total += amount
                        tracker.addPosition(change.uptimeMillis, total)
                        drag = Offset(total.x / density.density, total.y / density.density)
                    }
                }
                .clickable(role = Role.Button, onClickLabel = "Return to call") { DirectCallFeature.expand() }
                .semantics {
                    contentDescription = "Call with @${state.partner}, $status"
                    stateDescription = "Drag to move it"
                    customActions = listOf(
                        CustomAccessibilityAction("Return to call") {
                            DirectCallFeature.expand()
                            true
                        },
                    )
                },
        ) {
            if (webView != null) {
                key(webView) {
                    AndroidView(factory = { context -> webView.adoptContext(context) }, modifier = Modifier.fillMaxSize())
                }
            }
            if (!video || !remoteVideo) {
                Column(
                    Modifier
                        .fillMaxSize()
                        .background(Brush.verticalGradient(listOf(Color(0xFF333333), Color(0xFF141414))))
                        .padding(start = 6.dp, end = 6.dp, top = 6.dp, bottom = if (video) 40.dp else 44.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    CallAvatar(state.partner, size = if (video) 44 else 34)
                    if (video) {
                        Spacer(Modifier.height(4.dp))
                        Text(
                            "@${state.partner}",
                            color = Color.White,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    Spacer(Modifier.height(4.dp))
                    Text(
                        status,
                        color = Color.White.copy(alpha = .85f),
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Medium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            Row(
                Modifier.align(Alignment.BottomCenter).padding(bottom = 6.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                MiniCallButton(
                    icon = if (state.muted) "mic_off" else "mic",
                    label = if (state.muted) "Unmute" else "Mute",
                    fill = if (state.muted) Color.White else Color.Black.copy(alpha = .55f),
                    tint = if (state.muted) Color.Black else Color.White,
                ) { DirectCallFeature.run { it.toggleMute() } }
                MiniCallButton(icon = "call_end", label = "Hang up", fill = Fm.colors.danger, tint = Color.White) {
                    DirectCallFeature.run { it.hangUp() }
                }
            }
        }
    }
}

@Composable
private fun rememberMiniStatus(state: DirectCallView): String {
    val since = state.liveSince
    val timed = state.phase == DirectCallPhase.LIVE && !state.reconnecting && since != null
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    if (timed) {
        LaunchedEffect(since) {
            while (true) {
                now = System.currentTimeMillis()
                delay(1000 - now % 1000)
            }
        }
    }
    return when {
        timed && since != null -> CallMiniLayout.duration((now - since) / 1000)
        state.reconnecting -> "Reconnecting…"
        state.phase == DirectCallPhase.RINGING -> "Calling…"
        else -> "Connecting…"
    }
}

@Composable
private fun MiniCallButton(icon: String, label: String, fill: Color, tint: Color, onPressed: () -> Unit) {
    Box(
        Modifier
            .size(40.dp)
            .clip(CircleShape)
            .clickable(role = Role.Button, onClick = onPressed)
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) {
        Box(Modifier.size(30.dp).clip(CircleShape).background(fill), contentAlignment = Alignment.Center) {
            FmIcon(icon, size = 15.dp, tint = tint)
        }
    }
}

@Composable
private fun DraggableSelfView(webView: WebView, topClearance: Float, bottomClearance: Float) {
    val density = LocalDensity.current
    val prefs = remember { Services.get<PrefStore>() }
    var corner by remember { mutableStateOf(CallPreviewCorner.from(prefs?.getString(CallPreviewCorner.KEY))) }
    var drag by remember { mutableStateOf(Offset.Zero) }
    var dragging by remember { mutableStateOf(false) }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val width = maxWidth.value
        val height = maxHeight.value
        val top = with(density) { topClearance.toDp().value } + FmSpace.x2.value
        val bottom = if (bottomClearance > 0f) {
            height - with(density) { bottomClearance.toDp().value } + CallPreviewLayout.MARGIN
        } else {
            CallPreviewLayout.BOTTOM_CLEARANCE
        }
        val spot = CallPreviewLayout.center(corner, width, height, bottom, top)
        val scale = if (dragging) 1.05f else 1f
        val tileWidth = CallPreviewLayout.TILE_WIDTH * scale
        val tileHeight = CallPreviewLayout.TILE_HEIGHT * scale
        val left = spot.x + drag.x - tileWidth / 2
        val upper = spot.y + drag.y - tileHeight / 2
        LaunchedEffect(webView, left, upper, tileWidth, dragging) {
            webView.evaluateJavascript(
                "window.fmCallPlaceLocal && window.fmCallPlaceLocal($left, $upper, $tileWidth, $tileHeight, ${!dragging})",
                null,
            )
        }
        val tracker = remember { VelocityTracker() }
        Box(
            Modifier
                .offset(left.dp, upper.dp)
                .size(tileWidth.dp, tileHeight.dp)
                .semantics {
                    contentDescription = "Your video"
                    stateDescription = "Drag to move it to another corner"
                }
                .pointerInput(width, height, top, bottom) {
                    var total = Offset.Zero
                    detectDragGestures(
                        onDragStart = {
                            tracker.resetTracking()
                            total = Offset.Zero
                            dragging = true
                        },
                        onDragEnd = {
                            val velocity = tracker.calculateVelocity()
                            val next = CallPreviewLayout.settle(
                                corner,
                                drag.x,
                                drag.y,
                                velocity.x / density.density,
                                velocity.y / density.density,
                                width,
                                height,
                                bottom,
                                top,
                            )
                            corner = next
                            prefs?.putString(CallPreviewCorner.KEY, next.wire)
                            drag = Offset.Zero
                            dragging = false
                        },
                        onDragCancel = {
                            drag = Offset.Zero
                            dragging = false
                        },
                    ) { change, amount ->
                        change.consume()
                        total += amount
                        tracker.addPosition(change.uptimeMillis, total)
                        drag = Offset(total.x / density.density, total.y / density.density)
                    }
                },
        )
    }
}
