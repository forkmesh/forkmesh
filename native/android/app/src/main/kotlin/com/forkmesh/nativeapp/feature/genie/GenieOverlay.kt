package com.forkmesh.nativeapp.feature.genie

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.view.Surface
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.genie.GenieArena
import com.forkmesh.nativeapp.genie.GenieCrawl
import com.forkmesh.nativeapp.genie.GenieEdge
import com.forkmesh.nativeapp.genie.GenieGait
import com.forkmesh.nativeapp.genie.GenieGravity
import com.forkmesh.nativeapp.genie.GenieHintContext
import com.forkmesh.nativeapp.genie.GenieHints
import com.forkmesh.nativeapp.genie.GenieOutreachChannel
import com.forkmesh.nativeapp.genie.GeniePoint
import com.forkmesh.nativeapp.genie.GenieTrack
import com.forkmesh.nativeapp.genie.GenieVector
import com.forkmesh.nativeapp.model.PingCelebration
import com.forkmesh.nativeapp.shell.ShellBar
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.android.awaitFrame
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.random.Random

class GenieCrawler {
    companion object {
        const val SIDE = 92.0
        const val LOOSEN = 0.25
        const val EDGE_SLACK = 30.0
    }

    var gravity = GenieVector.DOWN
    var arena: GenieArena? = null
    var track: GenieTrack? = null
    var point = GeniePoint(0.0, 0.0)
    var surface = GenieEdge.Bottom
    var turn = 0.0
    var held = false
    var placed = false
    private var nextWander = 0.0
    private var lastKnock = 0.0

    fun place(arena: GenieArena, now: Double) {
        this.arena = arena
        if (!placed) {
            placed = true
            val saved = GenieSpiderState.spot()
            surface = saved?.surface ?: GenieEdge.Bottom
            point = if (GenieSpiderState.parked.value && saved != null) {
                GeniePoint(saved.x, saved.y)
            } else {
                arena.on(surface, saved?.let { GeniePoint(it.x, it.y) } ?: GeniePoint(arena.box.maxX, arena.box.maxY))
            }
            turn = surface.degrees
            nextWander = now + 3
            return
        }
        if (!held && track == null && !GenieSpiderState.parked.value) point = arena.on(surface, point)
    }

    fun position(now: Double): GeniePoint = track?.point(now) ?: point

    fun rotation(now: Double): Double = track?.rotation(now) ?: turn

    fun gait(now: Double): GenieGait = if (held) GenieGait(phase = now * 14, air = 1.0) else track?.gait(now) ?: GenieGait.STILL

    fun step(now: Double) {
        val current = track
        if (current != null && now >= current.end) {
            point = current.final ?: point
            surface = current.surface ?: surface
            turn = current.turn
            track = null
            GenieSpiderState.saveSpot(GenieSpot(point.x, point.y, surface))
        }
        val arena = arena ?: return
        if (held || track != null || GenieSpiderState.parked.value || !GenieSpiderState.wanders.value) return
        if (!GenieSpiderState.calm || now < nextWander) return
        val random = { Random.nextDouble() }
        val silk = if (GenieGravity.lowness(surface, gravity) < GenieGravity.CEILING && random() < 0.35) {
            GenieGravity.silk(point, surface, arena, gravity, 0.3 + 0.5 * random(), 1.0 + 2 * random(), random() < 0.4, GenieCrawl.SPEED, now, turn)
        } else {
            null
        }
        track = silk ?: run {
            val (target, goal) = GenieGravity.wander(point, surface, arena, gravity, random)
            GenieGravity.crawl(point, surface, target, goal, arena, gravity, GenieCrawl.SPEED, now, turn)
        }
        nextWander = (track?.end ?: now) + GenieGravity.WANDER_MIN + (GenieGravity.WANDER_MAX - GenieGravity.WANDER_MIN) * random()
    }

    fun grab(now: Double) {
        point = position(now)
        turn = rotation(now)
        track = null
        held = true
        if (GenieSpiderState.parked.value) GenieSpiderState.setParked(false)
    }

    fun drag(dx: Double, dy: Double) {
        point = GeniePoint(point.x + dx, point.y + dy)
    }

    fun release(velocity: GenieVector, width: Double, side: Double, now: Double) {
        held = false
        val arena = arena ?: return
        val speed = velocity.length
        if (speed < GenieGravity.TUCK_SPEED && (point.x < arena.box.minX + EDGE_SLACK || point.x > arena.box.maxX - EDGE_SLACK)) {
            val left = point.x < width / 2
            surface = if (left) GenieEdge.Left else GenieEdge.Right
            point = GeniePoint(if (left) side / 6 else width - side / 6, point.y.coerceIn(arena.box.minY, arena.box.maxY))
            turn = surface.degrees
            GenieSpiderState.setParked(true)
            GenieSpiderState.saveSpot(GenieSpot(point.x, point.y, surface))
            return
        }
        track = GenieGravity.fly(point, velocity, gravity, arena, now, turn)
        nextWander = (track?.end ?: now) + GenieGravity.WANDER_MIN
    }

    fun knock(push: GenieVector, now: Double) {
        val arena = arena ?: return
        if (held || track != null || GenieSpiderState.parked.value || now - lastKnock < 1.5) return
        if (hypot(push.dx, push.dy) < LOOSEN) return
        lastKnock = now
        track = GenieGravity.fly(point, GenieVector(-push.dx * 900, -push.dy * 900), gravity, arena, now, turn)
    }

    fun comeOut(now: Double) {
        GenieSpiderState.setParked(false)
        val arena = arena ?: return
        track = GenieGravity.fall(point, surface, gravity, arena, now, turn)
    }

    fun reset() {
        GenieSpiderState.reset()
        placed = false
        track = null
    }
}

private fun screenVector(x: Float, y: Float, rotation: Int): GenieVector = when (rotation) {
    Surface.ROTATION_90 -> GenieVector(y.toDouble(), x.toDouble())
    Surface.ROTATION_180 -> GenieVector(x.toDouble(), -y.toDouble())
    Surface.ROTATION_270 -> GenieVector(-y.toDouble(), -x.toDouble())
    else -> GenieVector(-x.toDouble(), y.toDouble())
}

@Composable
fun GenieOverlay() {
    val hidden by GenieSpiderState.hidden.collectAsState()
    if (hidden) return
    val crawler = remember { GenieCrawler() }
    var now by remember { mutableDoubleStateOf(System.nanoTime() / 1e9) }
    val context = LocalContext.current
    val view = LocalView.current
    LaunchedEffect(Unit) {
        while (true) {
            awaitFrame()
            val time = System.nanoTime() / 1e9
            val rate = when {
                crawler.held || crawler.track?.airborne(time) == true -> 60.0
                GenieSpiderState.cue.value != null -> 30.0
                else -> 24.0
            }
            if (time - now >= 1.0 / rate - 0.002) {
                crawler.step(time)
                now = time
            }
        }
    }
    DisposableEffect(Unit) {
        val sensors = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                val rotation = view.display?.rotation ?: Surface.ROTATION_0
                val vector = screenVector(event.values[0], event.values[1], rotation)
                when (event.sensor.type) {
                    Sensor.TYPE_GRAVITY -> crawler.gravity = GenieVector(vector.dx / SensorManager.GRAVITY_EARTH, vector.dy / SensorManager.GRAVITY_EARTH)
                    Sensor.TYPE_LINEAR_ACCELERATION ->
                        crawler.knock(GenieVector(vector.dx / SensorManager.GRAVITY_EARTH, vector.dy / SensorManager.GRAVITY_EARTH), System.nanoTime() / 1e9)
                }
            }

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        }
        sensors?.getDefaultSensor(Sensor.TYPE_GRAVITY)?.let { sensors.registerListener(listener, it, SensorManager.SENSOR_DELAY_GAME) }
        sensors?.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION)?.let { sensors.registerListener(listener, it, SensorManager.SENSOR_DELAY_GAME) }
        onDispose { sensors?.unregisterListener(listener) }
    }
    val celebration = Services.get<NotificationService>()?.celebration?.collectAsState()?.value
    val baseline = remember { celebration?.count ?: 0 }
    LaunchedEffect(celebration?.count) {
        val next = celebration ?: return@LaunchedEffect
        if (next.count <= baseline) return@LaunchedEffect
        val line = PingCelebration.badgeLine(next.ping) ?: return@LaunchedEffect
        GenieSpiderState.say(line)
        GenieSpiderState.react(GenieReaction.Cheer)
    }
    val scale by GenieSpiderState.scale.collectAsState()
    val mood by GenieSpiderState.mood.collectAsState()
    val cue by GenieSpiderState.cue.collectAsState()
    val line by GenieSpiderState.line.collectAsState()
    val offer by GenieOutreachCoach.offer.collectAsState()
    val parked by GenieSpiderState.parked.collectAsState()
    val unread = Services.get<NotificationService>()?.unreadCount?.collectAsState()?.value ?: 0
    val suggestions by ShellSuggestions.on.collectAsState()
    val autoSend by ShellAutoSend.on.collectAsState()
    var menu by remember { mutableStateOf(false) }
    var hint by remember { mutableStateOf(0) }
    var lastHint by remember { mutableDoubleStateOf(0.0) }
    val density = LocalDensity.current
    val haptics = FmHaptics.current
    val ceiling = with(density) { WindowInsets.statusBars.getTop(this).toDp() } + 56.dp
    val clearance = ShellBar.clearance()
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val width = maxWidth.value.toDouble()
        val height = maxHeight.value.toDouble()
        val side = GenieCrawler.SIDE * scale
        remember(width, height, ceiling, clearance, side) {
            GenieArena.of(width, height, ceiling.value.toDouble(), height - clearance.value, side).also { crawler.place(it, System.nanoTime() / 1e9) }
        }
        val sideDp = side.dp
        fun spot(time: Double): GeniePoint = crawler.position(time)
        Box(
            Modifier
                .offset {
                    val p = spot(now)
                    IntOffset(((p.x - side / 2) * density.density).roundToInt(), ((p.y - side / 2) * density.density).roundToInt())
                }
                .size(sideDp)
                .graphicsLayer { rotationZ = crawler.rotation(now).toFloat() }
                .semantics {
                    role = Role.Button
                    contentDescription = "Genie spider. Tap for a tip, touch and hold for more."
                    testTag = "genie"
                }
                .pointerInput(unread, suggestions, autoSend) {
                    detectTapGestures(
                        onTap = {
                            val time = System.nanoTime() / 1e9
                            if (time - lastHint > 60) hint = 0
                            val lines = GenieHints.lines(GenieHintContext(unread = unread, suggestions = suggestions, autoSendOn = autoSend))
                            GenieSpiderState.say(lines[hint % lines.size])
                            hint += 1
                            lastHint = time
                            GenieSpiderState.react(GenieReaction.Wave)
                            haptics.tap()
                        },
                        onLongPress = {
                            haptics.palette()
                            menu = true
                        },
                    )
                }
                .pointerInput(width, side) {
                    val tracker = VelocityTracker()
                    detectDragGestures(
                        onDragStart = {
                            tracker.resetTracking()
                            crawler.grab(System.nanoTime() / 1e9)
                            GenieSpiderState.react(GenieReaction.Giggle)
                        },
                        onDragEnd = {
                            val v = tracker.calculateVelocity()
                            crawler.release(GenieVector(v.x / density.density.toDouble(), v.y / density.density.toDouble()), width, side, System.nanoTime() / 1e9)
                        },
                        onDragCancel = { crawler.release(GenieVector.ZERO, width, side, System.nanoTime() / 1e9) },
                    ) { change, amount ->
                        change.consume()
                        tracker.addPosition(change.uptimeMillis, change.position)
                        crawler.drag(amount.x / density.density.toDouble(), amount.y / density.density.toDouble())
                    }
                },
        ) {
            Canvas(Modifier.fillMaxSize()) {
                val time = now
                val active = cue?.let { c -> (time - c.at).takeIf { it >= 0 && it < c.reaction.duration }?.let { c.reaction to it } }
                GeniePainter(
                    mood = mood, level = 0.0, t = time, reaction = active?.first, reactionAge = active?.second ?: 0.0,
                    leanX = crawler.gravity.dx.toFloat(), leanY = 0f, stride = crawler.gait(time),
                ).paint(this)
            }
            FmDropdownMenu(menu, { menu = false }) {
                GenieMenu(crawler, close = { menu = false }, parked = parked, time = { System.nanoTime() / 1e9 })
            }
        }
        offer?.let { current ->
            Box(
                Modifier.offset {
                    val p = spot(now)
                    val above = p.y > height / 2
                    val x = (p.x - 110).coerceIn(8.0, width - 228)
                    val y = if (above) p.y - side / 2 - 104 else p.y + side / 2 + 8
                    IntOffset((x * density.density).roundToInt(), (y * density.density).roundToInt())
                },
            ) { GenieOutreachCard(current) }
        }
        AnimatedVisibility(
            visible = line != null,
            enter = fadeIn(),
            exit = fadeOut(),
            modifier = Modifier.align(Alignment.TopCenter).padding(top = ceiling + FmSpace.x2),
        ) {
            Text(
                line.orEmpty(),
                Modifier
                    .widthIn(max = 320.dp)
                    .clip(RoundedCornerShape(18.dp))
                    .background(Brush.verticalGradient(listOf(GenieInk.night, GenieInk.dusk)))
                    .border(1.dp, GenieInk.glow.copy(alpha = 0.5f), RoundedCornerShape(18.dp))
                    .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2)
                    .semantics {
                        liveRegion = LiveRegionMode.Polite
                        testTag = "genie-line"
                    },
                color = Color.White,
                fontSize = 15.sp,
                textAlign = TextAlign.Center,
            )
        }
        GenieTalkBubble(Modifier.align(Alignment.BottomCenter).padding(bottom = clearance + FmSpace.x2))
    }
}

@Composable
private fun GenieMenu(crawler: GenieCrawler, close: () -> Unit, parked: Boolean, time: () -> Double) {
    val outreachOff by GenieOutreachCoach.off.collectAsState()
    val wanders by GenieSpiderState.wanders.collectAsState()
    FmDropdownMenuItem("Talk", { close(); GenieTalkState.open() }, icon = "mic")
    FmDropdownMenuItem("Surprise me", { close(); GenieSpiderState.react(GenieReaction.entries.random()) }, icon = "auto_awesome")
    FmDropdownMenuItem("Someone to reach out to", { close(); GenieOutreachCoach.suggestNow() }, icon = "mail", enabled = !outreachOff)
    FmDropdownMenuItem(
        if (outreachOff) "Suggest people again" else "Stop suggesting people",
        { close(); GenieOutreachCoach.setOff(!outreachOff) },
        icon = "person_outline",
    )
    FmDropdownMenuItem(if (wanders) "Stay put" else "Crawl around", { close(); GenieSpiderState.setWanders(!wanders) }, icon = "pause")
    if (parked) {
        FmDropdownMenuItem("Come out", { close(); crawler.comeOut(time()) }, icon = "visibility")
    }
    FmDropdownMenuItem("Reset size and place", { close(); crawler.reset() }, icon = "refresh")
    FmDropdownMenuItem("Hide the spider", { close(); GenieSpiderState.setHidden(true) }, icon = "visibility_off")
}

@Composable
private fun GenieOutreachCard(offer: GenieOutreachOffer) {
    val pick = offer.pick
    val writing = offer.stage == GenieOfferStage.Writing
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.semantics { testTag = "genie-outreach" }) {
        Box(
            Modifier
                .size(60.dp)
                .clip(CircleShape)
                .border(2.dp, GenieInk.glow, CircleShape)
                .clickable(enabled = !writing) { GenieOutreachCoach.write() }
                .semantics {
                    role = Role.Button
                    contentDescription = if (writing) "Writing to ${pick.contact.displayName}" else pick.caption
                    testTag = "genie-outreach-write"
                }
                .graphicsLayer { alpha = if (writing) 0.7f else 1f },
            contentAlignment = Alignment.Center,
        ) {
            val image = offer.image
            if (image != null) {
                Image(image, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
            } else {
                Box(Modifier.fillMaxSize().background(Brush.linearGradient(listOf(GenieInk.violet, GenieInk.dusk))), contentAlignment = Alignment.Center) {
                    Text(pick.contact.initials, color = Color.White, fontWeight = FontWeight.Bold, fontSize = 20.sp)
                }
            }
        }
        Row(
            Modifier
                .widthIn(max = 220.dp)
                .clip(CircleShape)
                .background(Brush.verticalGradient(listOf(GenieInk.night, GenieInk.dusk)))
                .border(1.dp, GenieInk.glow.copy(alpha = 0.5f), CircleShape)
                .padding(horizontal = FmSpace.x2, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
        ) {
            FmIcon(if (pick.channel == GenieOutreachChannel.Email) "mail" else "chat_bubble_outline", size = 12.dp, tint = GenieInk.glow)
            val name = pick.firstName.ifEmpty { pick.contact.displayName }
            Text(
                if (writing) "Writing to $name…" else pick.caption,
                color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis,
            )
            if (!writing) {
                Box(
                    Modifier
                        .size(18.dp)
                        .clip(CircleShape)
                        .background(Color.White.copy(alpha = 0.14f))
                        .clickable { GenieOutreachCoach.dismiss() }
                        .semantics {
                            role = Role.Button
                            contentDescription = "Not now"
                            testTag = "genie-outreach-close"
                        },
                    contentAlignment = Alignment.Center,
                ) { FmIcon("close", size = 11.dp, tint = Color.White.copy(alpha = 0.8f)) }
            }
        }
    }
}

