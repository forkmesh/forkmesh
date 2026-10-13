package com.forkmesh.nativeapp.feature.agentwatch

import com.forkmesh.nativeapp.agentwatch.AgentWatchDefaults
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.agentwatch.AgentMotion
import com.forkmesh.nativeapp.agentwatch.AgentWatchController
import com.forkmesh.nativeapp.agentwatch.AgentWatchTarget
import com.forkmesh.nativeapp.agentwatch.ControlWatchRoster
import com.forkmesh.nativeapp.agentwatch.WatchFieldLayout
import com.forkmesh.nativeapp.agentwatch.WatchPoint
import com.forkmesh.nativeapp.agentwatch.WatchState
import com.forkmesh.nativeapp.agentwatch.WatchedAgent
import com.forkmesh.nativeapp.agentwatch.watchCardLines
import com.forkmesh.nativeapp.agentwatch.watchFaceName
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.drop

internal object WatchPrefs {
    const val SOUND = "agentWatch/sound"
    const val FEEL = "agentWatch/feel"
    const val HIDDEN = "agentWatch/hidden"
    const val MUTED = "agentWatch/muted"
    const val BACKDROP = "agentWatch/backdrop"
    const val LIST = "agentWatch/sounds"
    const val WORKING_ONLY = "agentWatch/workingOnly"
}

internal class AgentWatchScreen(
    private val targets: List<AgentWatchTarget>,
    private val heading: String,
    private val debounce: Double = 0.8,
    private val roster: ControlWatchRoster? = null,
) : Screen() {
    override val title: String get() = heading

    private var controller: AgentWatchController? = null
    private var output: AndroidAgentWatchOutput? = null
    private val leds = WatchLeds()
    private var hidden by mutableStateOf<Set<AgentMotion>>(emptySet())
    private var backdrop by mutableStateOf(WatchBackdrop.space)
    private var showSounds by mutableStateOf(true)
    private var selected by mutableStateOf<WatchedAgent?>(null)
    private var revision by mutableIntStateOf(0)
    private var frame by mutableIntStateOf(0)
    private var animating by mutableStateOf(true)
    private var menuOpen by mutableStateOf(false)

    override fun onDisposed() {
        roster?.stop()
        controller?.dispose()
        output?.release()
        controller = null
        output = null
    }

    private fun make(context: android.content.Context, prefs: Prefs): AgentWatchController {
        controller?.let { return it }
        val out = AndroidAgentWatchOutput(context.applicationContext)
        output = out
        val made = AgentWatchController(targets + (roster?.targets() ?: emptyList()), out, AndroidWatchClock(), backgroundWatchLoader(), debounce)
        fun byLabel(key: String): Set<AgentMotion> {
            val labels = prefs.getStringList(key)?.toSet() ?: emptySet()
            return AgentMotion.entries.filter { it.label in labels }.toSet()
        }
        made.sound = prefs.getBool(WatchPrefs.SOUND) ?: true
        val scale = runCatching {
            android.provider.Settings.Global.getFloat(context.contentResolver, android.provider.Settings.Global.ANIMATOR_DURATION_SCALE, 1f)
        }.getOrNull()
        val reduced = AgentWatchDefaults.reducedMotion(scale, android.animation.ValueAnimator.areAnimatorsEnabled())
        made.feel = AgentWatchDefaults.feel(prefs.getBool(WatchPrefs.FEEL), reduced)
        made.workingOnly = prefs.getBool(WatchPrefs.WORKING_ONLY) ?: false
        for (motion in byLabel(WatchPrefs.MUTED)) made.setMuted(motion, true)
        hidden = byLabel(WatchPrefs.HIDDEN)
        backdrop = WatchBackdrop.entries.firstOrNull { it.label == prefs.getString(WatchPrefs.BACKDROP) } ?: WatchBackdrop.space
        showSounds = prefs.getBool(WatchPrefs.LIST) ?: true
        made.addListener {
            revision++
            animating = true
        }
        controller = made
        made.start()
        roster?.let { follow(it, made) }
        return made
    }

    private fun follow(roster: ControlWatchRoster, made: AgentWatchController) {
        val main = Handler(Looper.getMainLooper())
        roster.feeds.onChange = { main.post { made.ring() } }
        roster.watch { fresh ->
            main.post {
                if (made.add(fresh) > 0) animating = true
                made.ring()
            }
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val prefs = remember { Prefs(context.applicationContext) }
        val controller = remember { make(context, prefs) }
        val faces = remember {
            WatchFaces { name ->
                runCatching { context.assets.open("agent_faces/$name.png").use { BitmapFactory.decodeStream(it) } }.getOrNull()
            }
        }
        LaunchedEffect(controller) {
            var first = true
            AppLifecycle.foreground.collect { shown ->
                if (shown) {
                    if (!first) controller.resume()
                    animating = true
                } else {
                    controller.pause()
                }
                first = false
            }
        }
        LaunchedEffect(controller) {
            AgentsFeature.agentBeats.collect { controller.ring() }
        }
        LaunchedEffect(controller) {
            AgentsFeature.liveSignal.drop(1).collect { controller.ring() }
        }
        LaunchedEffect(controller) {
            val roster = roster ?: return@LaunchedEffect
            AgentsFeature.listSignal.drop(1).collect { roster.refresh() }
        }
        LaunchedEffect(controller) {
            val feeds = roster?.feeds ?: return@LaunchedEffect
            while (true) {
                delay(3_000)
                if (feeds.lagging) controller.ring()
            }
        }
        LaunchedEffect(animating) {
            while (animating) {
                withFrameNanos { }
                controller.advance()
                frame++
            }
        }
        Column(
            Modifier
                .fillMaxSize()
                .background(Color(WATCH_BACKGROUND))
                .statusBarsPadding(),
        ) {
            TopBar(navigator, controller, prefs)
            BoxWithConstraints(Modifier.fillMaxSize()) {
                val wide = maxWidth >= 700.dp
                val listWidth = (maxWidth * 0.24f).coerceIn(230.dp, 330.dp)
                val fieldHeight = maxHeight * 0.6f
                if (!showSounds) {
                    Field(controller, faces, Modifier.fillMaxSize())
                } else if (wide) {
                    Row(Modifier.fillMaxSize()) {
                        Field(controller, faces, Modifier.weight(1f).fillMaxHeight())
                        WatchSoundList(
                            controller, frame, hidden, { m, sound -> toggleKind(m, sound, prefs) },
                            Modifier.width(listWidth).fillMaxHeight(),
                        )
                    }
                } else {
                    Column(Modifier.fillMaxSize()) {
                        Field(controller, faces, Modifier.fillMaxWidth().height(fieldHeight))
                        WatchSoundList(controller, frame, hidden, { m, sound -> toggleKind(m, sound, prefs) }, Modifier.fillMaxWidth().weight(1f))
                    }
                }
            }
        }
    }

    private fun toggleKind(motion: AgentMotion, sound: Boolean, prefs: Prefs) {
        val controller = controller ?: return
        if (sound) {
            controller.setMuted(motion, motion !in controller.muted)
            prefs.putStringList(WatchPrefs.MUTED, controller.muted.map { it.label }.sorted())
        } else {
            hidden = if (motion in hidden) hidden - motion else hidden + motion
            prefs.putStringList(WatchPrefs.HIDDEN, hidden.map { it.label }.sorted())
        }
        animating = true
    }

    @Composable
    private fun TopBar(navigator: Navigator, controller: AgentWatchController, prefs: Prefs) {
        revision.let { }
        val fg = Color(WATCH_FOREGROUND)
        Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp, vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            FmIconButton("arrow_back_rounded", "Back", onPressed = { navigator.pop() }, color = fg)
            Text(heading, Modifier.weight(1f), color = fg, fontSize = 17.sp, fontWeight = FontWeight.SemiBold, maxLines = 1)
            FmIconButton(
                if (controller.sound) "volume_up" else "volume_off",
                if (controller.sound) "Stop hearing everything" else "Hear everything",
                onPressed = {
                    controller.sound = !controller.sound
                    prefs.putBool(WatchPrefs.SOUND, controller.sound)
                },
                color = fg,
            )
            FmIconButton(
                if (controller.feel) "vibration" else "mobile_off",
                if (controller.feel) "Stop vibrating" else "Feel the agents",
                onPressed = {
                    controller.feel = !controller.feel
                    prefs.putBool(WatchPrefs.FEEL, controller.feel)
                },
                color = fg,
            )
            Box {
                FmIconButton("more_vert", "View", onPressed = { menuOpen = true }, color = fg)
                DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }, modifier = Modifier.fmTapSounds()) {
                    DropdownMenuItem(
                        text = { Text((if (showSounds) "✓ " else "") + "Sounds") },
                        onClick = {
                            showSounds = !showSounds
                            prefs.putBool(WatchPrefs.LIST, showSounds)
                            menuOpen = false
                        },
                    )
                    DropdownMenuItem(
                        text = { Text((if (controller.workingOnly) "✓ " else "") + "Working only") },
                        onClick = {
                            controller.workingOnly = !controller.workingOnly
                            prefs.putBool(WatchPrefs.WORKING_ONLY, controller.workingOnly)
                            animating = true
                            menuOpen = false
                        },
                    )
                    for (choice in WatchBackdrop.entries) {
                        DropdownMenuItem(
                            text = { Text((if (backdrop == choice) "✓ " else "") + "Backdrop: ${choice.label}") },
                            onClick = {
                                backdrop = choice
                                prefs.putString(WatchPrefs.BACKDROP, choice.label)
                                menuOpen = false
                            },
                        )
                    }
                }
            }
        }
    }

    @Composable
    private fun Field(controller: AgentWatchController, faces: WatchFaces, modifier: Modifier) {
        val density = LocalDensity.current.density
        val text = remember(density) { WatchText(density) }
        BoxWithConstraints(modifier) {
            val width = maxWidth.value.toDouble()
            val height = maxOf(maxHeight.value.toDouble(), WatchFieldLayout.heightFor(width, controller.agents))
            val nodeLabel = if (roster == null && targets.isNotEmpty() && targets.all { it.subtitle == "On this phone" }) "this phone" else "your desktop"
            Box(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                Canvas(
                    Modifier
                        .fillMaxWidth()
                        .height(height.dp)
                        .semantics { contentDescription = headline(controller) }
                        .pointerInput(controller) {
                            detectTapGestures(
                                onTap = { at -> tap(controller, width, height, WatchPoint(at.x / density.toDouble(), at.y / density.toDouble())) },
                                onDoubleTap = { at -> doubleTap(controller, width, height, WatchPoint(at.x / density.toDouble(), at.y / density.toDouble())) },
                            )
                        },
                ) {
                    frame.let { }
                    revision.let { }
                    drawIntoCanvas { composeCanvas ->
                        val canvas = composeCanvas.nativeCanvas
                        canvas.save()
                        canvas.scale(density, density)
                        WatchSky.paint(canvas, width, height, backdrop)
                        val layout = WatchFieldLayout(width, height, controller.agents)
                        WatchFieldPainter(
                            controller, layout, leds,
                            { agent -> faces.face(watchFaceName(agent.target.provider, agent.target.model)) },
                            hidden, selected, nodeLabel, text, controller.now,
                        ).paint(canvas, width, height)
                        canvas.restore()
                        if (!controller.moving && !WatchFieldPainter.ledsMoving(controller, leds, layout)) animating = false
                    }
                }
            }
        }
    }

    private fun tap(controller: AgentWatchController, width: Double, height: Double, at: WatchPoint) {
        val layout = WatchFieldLayout(width, height, controller.agents)
        val agent = layout.agentAt(at, controller.now) { controller.shown(it) }
        selected = if (agent === selected) null else agent
        if (agent != null) controller.greet(agent)
        animating = true
    }

    private fun doubleTap(controller: AgentWatchController, width: Double, height: Double, at: WatchPoint) {
        val layout = WatchFieldLayout(width, height, controller.agents)
        val agent = layout.agentAt(at, controller.now) { controller.shown(it) } ?: return
        controller.toggleListening(agent)
        selected = agent
        animating = true
    }

    private fun headline(controller: AgentWatchController): String {
        val agents = controller.agents
        if (agents.isEmpty()) return "No agents to watch. Start one, or open a session to replay it."
        val working = agents.count { it.state == WatchState.running || it.state == WatchState.starting }
        val waiting = agents.count { it.state == WatchState.waiting }
        val parts = arrayListOf(
            if (controller.loading) "Reading ${if (agents.size == 1) "the run" else "the runs"}"
            else "${agents.size} ${if (agents.size == 1) "agent" else "agents"}",
        )
        if (working > 0) parts += "$working working"
        if (waiting > 0) parts += "$waiting waiting on you"
        val lines = arrayListOf(parts.joinToString(", "))
        selected?.let { lines += watchCardLines(controller, it) }
        return lines.joinToString("\n")
    }
}
