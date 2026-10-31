package com.forkmesh.nativeapp.feature.pocket

import android.app.Activity
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.spring
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.glasses.GlassesCompass
import com.forkmesh.nativeapp.feature.glasses.GlassesRadar
import com.forkmesh.nativeapp.feature.glasses.GlassesWatchService
import com.forkmesh.nativeapp.feature.health.StepsWalkBadge
import com.forkmesh.nativeapp.feature.health.WalkActivity
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.glasses.GlassesRadarLayout
import com.forkmesh.nativeapp.health.HealthPrefs
import com.forkmesh.nativeapp.inbox.InboxItem
import com.forkmesh.nativeapp.inbox.InboxSource
import com.forkmesh.nativeapp.pocket.PocketText
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import java.util.Locale
import kotlin.math.roundToInt
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

object PocketMode {
    private val lockedState = MutableStateFlow(false)
    val locked: StateFlow<Boolean> = lockedState

    fun lock() {
        lockedState.value = true
    }

    fun unlock() {
        lockedState.value = false
    }
}

@Composable
fun PocketModeRow(onClose: () -> Unit) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val walking = WalkActivity.moving
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 46.dp)
            .clickable {
                haptics.selection()
                onClose()
                haptics.commit()
                PocketMode.lock()
            }
            .semantics(mergeDescendants = true) {
                role = Role.Button
                contentDescription = "${PocketText.hubTitle(walking)}. ${PocketText.HUB_HINT}"
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        com.forkmesh.nativeapp.ui.kit.FmMenuTile { PulsingIcon(PocketText.hubIcon(walking), c.accent, pulsing = walking) }
        Spacer(Modifier.width(FmSpace.x3))
        Text(
            PocketText.hubTitle(walking),
            Modifier.weight(1f),
            style = Fm.text.bodyLarge.copy(fontSize = Fm.text.bodyMedium.fontSize * 1.15f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        FmIcon("chevron_right", size = 18.dp, tint = c.textTertiary)
    }
}

@Composable
private fun PulsingIcon(name: String, tint: Color, pulsing: Boolean) {
    val pulse = remember { Animatable(1f) }
    LaunchedEffect(pulsing) {
        if (!pulsing) {
            pulse.snapTo(1f)
            return@LaunchedEffect
        }
        while (true) {
            pulse.animateTo(0.35f, spring(stiffness = 40f))
            pulse.animateTo(1f, spring(stiffness = 40f))
        }
    }
    FmIcon(name, Modifier.alpha(pulse.value), size = 20.dp, tint = tint, outlined = true)
}

@Composable
fun PocketLockOverlay() {
    val locked by PocketMode.locked.collectAsState()
    AnimatedVisibility(locked, enter = fadeIn(), exit = fadeOut()) {
        PocketLockScreen()
    }
}

@Composable
fun PocketLockScreen() {
    val haptics = FmHaptics.current
    BackHandler(enabled = true) {}
    ImmersiveWhileShown()
    Column(
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {}
            .systemBarsPadding()
            .padding(horizontal = FmSpace.x5, vertical = FmSpace.x4)
            .semantics { contentDescription = "Pocket mode lock screen" },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(FmSpace.x5),
    ) {
        PocketClock()
        Spacer(Modifier.weight(1f))
        PocketGlasses()
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x4), verticalAlignment = Alignment.Top) {
            PocketPings(Modifier.weight(1f))
            PocketWalking(Modifier.weight(1f))
        }
        Spacer(Modifier.weight(1f))
        PocketUnlockSlider {
            haptics.success()
            PocketMode.unlock()
        }
    }
}

@Composable
private fun ImmersiveWhileShown() {
    val view = LocalView.current
    val activity = LocalContext.current as? Activity
    DisposableEffect(activity) {
        val window = activity?.window
        val controller = window?.let { WindowCompat.getInsetsController(it, view) }
        val previous = controller?.systemBarsBehavior
        controller?.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        controller?.hide(WindowInsetsCompat.Type.systemBars())
        view.keepScreenOn = true
        onDispose {
            controller?.show(WindowInsetsCompat.Type.systemBars())
            if (previous != null) controller.systemBarsBehavior = previous
            view.keepScreenOn = false
        }
    }
}

@Composable
private fun rememberMinuteClock(): Long {
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) {
            val current = System.currentTimeMillis()
            now = current
            delay(60_000 - current % 60_000)
        }
    }
    return now
}

private val dayFormat = DateTimeFormatter.ofPattern("EEEE, MMM d", Locale.getDefault())
private val timeFormat = DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT).withLocale(Locale.getDefault())

@Composable
private fun PocketClock() {
    val now = rememberMinuteClock()
    val time = Instant.ofEpochMilli(now).atZone(ZoneId.systemDefault())
    Column(Modifier.padding(top = FmSpace.x5).semantics(mergeDescendants = true) {}, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(dayFormat.format(time), style = Fm.text.titleMedium.copy(color = Color.White.copy(alpha = 0.7f), fontWeight = FontWeight.SemiBold))
        Text(
            timeFormat.format(time),
            style = Fm.text.displayLarge.copy(
                color = Color.White,
                fontSize = 72.sp,
                lineHeight = 80.sp,
                fontWeight = FontWeight.SemiBold,
                fontFamily = FontFamily.Default,
                fontFeatureSettings = "tnum",
            ),
        )
    }
}

@Composable
private fun PocketGlasses() {
    val c = Fm.colors
    val state by GlassesWatchService.state.collectAsState()
    if (state.watching) {
        val heading by GlassesCompass.heading.collectAsState()
        val nearby = state.nearby.size
        val heard = nearby + state.others.size
        val alarmed = PocketText.alarmed(state.ringing, nearby)
        val blips = GlassesRadarLayout.blips(state.others, state.nearby, state.homing, heading)
        Column(
            Modifier.semantics(mergeDescendants = true) {},
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            GlassesRadar(blips, if (alarmed) c.danger else c.success, Color.White.copy(alpha = 0.75f), c.danger, size = 150.dp)
            Text(
                PocketText.glassesTitle(nearby),
                style = Fm.text.headlineSmall.copy(color = if (alarmed) c.danger else Color.White, fontWeight = FontWeight.Bold),
                textAlign = TextAlign.Center,
            )
            Text(PocketText.devicesHeard(heard), style = Fm.text.bodyMedium.copy(color = Color.White.copy(alpha = 0.6f)))
        }
    } else {
        Column(
            Modifier.semantics(mergeDescendants = true) {},
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            FmIcon("visibility", size = 64.dp, tint = Color.White.copy(alpha = 0.4f), outlined = true)
            Text("Glasses Alert is off", style = Fm.text.titleLarge.copy(color = Color.White.copy(alpha = 0.6f), fontWeight = FontWeight.SemiBold))
        }
    }
}

@Composable
private fun rememberUnreadPings(): List<InboxItem> {
    var tick by remember { mutableLongStateOf(0L) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(5_000)
            tick++
        }
    }
    return remember(tick) {
        InboxFeature.model?.items.orEmpty().filter { it.source != InboxSource.Task && it.unread }
    }
}

@Composable
private fun PocketCard(modifier: Modifier, label: String, content: @Composable () -> Unit) {
    Column(
        modifier
            .background(Color.White.copy(alpha = 0.08f), RoundedCornerShape(24.dp))
            .padding(FmSpace.x3)
            .semantics(mergeDescendants = true) { contentDescription = label },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        content()
    }
}

@Composable
private fun PocketPings(modifier: Modifier) {
    val c = Fm.colors
    val unread = rememberUnreadPings()
    val title = PocketText.pingsTitle(unread.size)
    PocketCard(modifier, title) {
        FmIcon(
            if (unread.isNotEmpty()) "notifications_active" else "notifications",
            size = 56.dp,
            tint = if (unread.isNotEmpty()) c.danger else Color.White.copy(alpha = 0.4f),
        )
        Text(title, style = Fm.text.titleLarge.copy(color = Color.White, fontWeight = FontWeight.Bold), textAlign = TextAlign.Center)
        unread.take(3).forEach { ping ->
            Text(
                ping.title,
                style = Fm.text.labelSmall.copy(color = Color.White.copy(alpha = 0.7f)),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun PocketWalking(modifier: Modifier) {
    val c = Fm.colors
    val moving = WalkActivity.moving
    val stepping = WalkActivity.stepping
    val detecting = HealthPrefs.bool(Services.get<PrefStore>(), HealthPrefs.STEPS)
    val title = PocketText.walkingTitle(moving, stepping, WalkActivity.gaitText, detecting)
    PocketCard(modifier, title) {
        if (stepping) {
            StepsWalkBadge(WalkActivity.cadence, WalkActivity.running, 76.dp)
        } else {
            Box(Modifier.height(76.dp), contentAlignment = Alignment.Center) {
                FmIcon(
                    if (moving) "directions_walk" else "accessibility_new",
                    size = 64.dp,
                    tint = if (moving) c.accent else Color.White.copy(alpha = 0.4f),
                )
            }
        }
        Text(title, style = Fm.text.titleLarge.copy(color = Color.White, fontWeight = FontWeight.Bold), textAlign = TextAlign.Center)
        if (stepping) {
            Text(
                PocketText.cadence(WalkActivity.stepsPerMinute),
                style = Fm.text.labelSmall.copy(color = Color.White.copy(alpha = 0.7f), fontFeatureSettings = "tnum"),
            )
        }
    }
}

@Composable
fun PocketUnlockSlider(onUnlock: () -> Unit) {
    val knob = 60.dp
    val scope = rememberCoroutineScope()
    val drag = remember { Animatable(0f) }
    val density = LocalDensity.current
    BoxWithConstraints(
        Modifier
            .fillMaxWidth()
            .height(knob + 8.dp)
            .clearAndSetSemantics {
                contentDescription = "Slide to unlock"
                role = Role.Button
                onClick {
                    onUnlock()
                    true
                }
            },
    ) {
        val travel = with(density) { (maxWidth - knob - 8.dp).toPx() }.coerceAtLeast(1f)
        Box(Modifier.fillMaxSize().background(Color.White.copy(alpha = 0.12f), CircleShape))
        Text(
            "slide to unlock",
            Modifier.align(Alignment.Center),
            style = Fm.text.titleLarge.copy(
                color = Color.White.copy(alpha = PocketText.hintAlpha(drag.value.toDouble(), travel.toDouble()).toFloat()),
                fontWeight = FontWeight.Medium,
            ),
        )
        Box(
            Modifier
                .padding(4.dp)
                .offset { IntOffset(drag.value.roundToInt(), 0) }
                .size(knob)
                .background(Color.White, CircleShape)
                .pointerInput(travel) {
                    detectHorizontalDragGestures(
                        onDragEnd = {
                            if (PocketText.unlocks(drag.value.toDouble(), travel.toDouble())) {
                                scope.launch { drag.snapTo(travel) }
                                onUnlock()
                            } else {
                                scope.launch { drag.animateTo(0f, spring(dampingRatio = 0.7f, stiffness = 320f)) }
                            }
                        },
                        onDragCancel = { scope.launch { drag.animateTo(0f, spring(dampingRatio = 0.7f, stiffness = 320f)) } },
                    ) { change, amount ->
                        change.consume()
                        scope.launch { drag.snapTo((drag.value + amount).coerceIn(0f, travel)) }
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("keyboard_double_arrow_right", size = 20.dp, tint = Color.Black)
        }
    }
}
