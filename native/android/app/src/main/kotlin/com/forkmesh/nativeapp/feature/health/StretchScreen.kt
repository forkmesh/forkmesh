package com.forkmesh.nativeapp.feature.health

import android.content.Context
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.health.HealthApi
import com.forkmesh.nativeapp.health.StretchLibrary
import com.forkmesh.nativeapp.health.StretchMath
import com.forkmesh.nativeapp.health.StretchModel
import com.forkmesh.nativeapp.health.StretchRecentRow
import com.forkmesh.nativeapp.health.StretchRoutine
import com.forkmesh.nativeapp.health.StretchRun
import com.forkmesh.nativeapp.health.StretchSpeaker
import com.forkmesh.nativeapp.health.StretchStep
import com.forkmesh.nativeapp.health.StretchText
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import kotlin.math.min
import kotlin.math.roundToInt

class StretchVoice(context: Context) : StretchSpeaker {
    private var engine: TextToSpeech? = null
    private var ready = false
    private val queued = ArrayList<Pair<String, Boolean>>()
    private var current: String? = null
    private var interrupted: String? = null

    init {
        engine = TextToSpeech(context.applicationContext) { status ->
            ready = status == TextToSpeech.SUCCESS
            if (ready) {
                engine?.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
                    override fun onStart(utteranceId: String?) {}

                    override fun onDone(utteranceId: String?) {
                        if (utteranceId == current) current = null
                    }

                    @Deprecated("Deprecated in Java")
                    override fun onError(utteranceId: String?) {
                        if (utteranceId == current) current = null
                    }
                })
                val pending = ArrayList(queued)
                queued.clear()
                pending.forEach { say(it.first, it.second) }
            }
        }
    }

    override fun say(text: String, interrupt: Boolean) {
        if (text.isEmpty()) return
        val tts = engine
        if (tts == null || !ready) {
            queued.add(text to interrupt)
            return
        }
        interrupted = null
        current = text
        tts.speak(text, if (interrupt) TextToSpeech.QUEUE_FLUSH else TextToSpeech.QUEUE_ADD, null, text)
    }

    override fun pause() {
        val text = current ?: return
        interrupted = text
        engine?.stop()
    }

    override fun resume() {
        val text = interrupted ?: return
        interrupted = null
        say(text, true)
    }

    override fun stop() {
        interrupted = null
        current = null
        queued.clear()
        engine?.stop()
    }

    fun shutdown() {
        stop()
        engine?.shutdown()
        engine = null
    }
}

class StretchScreen : Screen() {
    override val title: String get() = "Stretch Break"

    private val model: StretchModel? by lazy {
        Services.get<ApiService>()?.let { StretchModel(HealthApi(it).stretch(), Services.get<PrefStore>()) }
    }

    private var rev by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model
        val scope = rememberCoroutineScope()
        val context = LocalContext.current
        val view = LocalView.current
        fun act(block: (StretchModel) -> Unit) {
            val current = model ?: return
            scope.launch {
                withContext(Dispatchers.IO) { block(current) }
                rev++
            }
        }
        val voice = remember { StretchVoice(context) }
        DisposableEffect(model) {
            model?.speaker = voice
            onDispose {
                val current = model
                if (current?.run?.finished == false) {
                    AppLifecycle.scope.launch(Dispatchers.IO) { current.stop() }
                }
                voice.shutdown()
                view.keepScreenOn = false
            }
        }
        LaunchedEffect(Unit) {
            val current = model ?: return@LaunchedEffect
            withContext(Dispatchers.IO) { current.load() }
            rev++
        }
        val inSession = model?.inSession == true && rev >= 0
        val running = model?.run?.let { !it.finished } == true && rev >= 0
        DisposableEffect(running) {
            view.keepScreenOn = running
            onDispose { }
        }
        LaunchedEffect(inSession) {
            while (isActive && inSession && model?.inSession == true) {
                delay(100)
                withContext(Dispatchers.IO) { model?.tick() }
                rev++
            }
        }
        LaunchedEffect(model) {
            AppLifecycle.foreground.collect { foreground ->
                val current = model ?: return@collect
                if (!foreground) {
                    if (current.run?.finished == false) {
                        current.pause()
                        rev++
                    }
                } else if (!current.inSession) {
                    withContext(Dispatchers.IO) { current.load() }
                    rev++
                }
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator) {
                if (!inSession) FmIconButton(icon = "refresh", label = "Refresh", onPressed = { act { it.load() } })
            }
            if (model == null) {
                FmEmptyState(icon = "self_improvement", title = StretchModel.UNAVAILABLE_TITLE)
            } else {
                StretchBody(model, rev, ::act)
            }
        }
    }
}

@Composable
private fun StretchBody(model: StretchModel, rev: Int, act: ((StretchModel) -> Unit) -> Unit) {
    var confirmUninstall by remember { mutableStateOf(false) }
    var deleting by remember { mutableStateOf<String?>(null) }
    val run = model.run
    val library = model.library
    when {
        run != null && library != null -> StretchSession(model, rev, run, library, act)
        model.showsSpinner -> FmSkeletonList()
        model.showsError -> FmEmptyState(icon = "self_improvement", title = StretchModel.UNAVAILABLE_TITLE, message = model.error)
        model.showsInstall -> StretchInstall(model, rev, act)
        model.overview != null -> StretchDashboard(model, rev, act, { confirmUninstall = true }, { deleting = it })
    }
    if (deleting != null) {
        FmConfirmDialog(
            title = "Remove this session?",
            body = "It is removed from your account on every device.",
            confirmLabel = "Remove",
            destructive = true,
            onDismissRequest = { deleting = null },
            onConfirm = {
                val id = deleting.orEmpty()
                deleting = null
                act { it.deleteSession(id) }
            },
        )
    }
    if (confirmUninstall) {
        FmConfirmDialog(
            title = "Uninstall Stretch Break?",
            body = "Your sessions and settings are deleted from the web dashboard, the desktop app, and this app.",
            confirmLabel = "Uninstall",
            destructive = true,
            onDismissRequest = { confirmUninstall = false },
            onConfirm = {
                confirmUninstall = false
                act { if (it.uninstall()) removeHealthPackageFromAccountNow("stretch") }
            },
        )
    }
}

@Composable
private fun StretchNotice(model: StretchModel, rev: Int) {
    var dismissed by remember(rev) { mutableStateOf(false) }
    val notice = model.notice
    if (notice != null && !dismissed) {
        FmInlineStatusLine(
            message = notice.message,
            isError = notice.isError,
            onDismiss = {
                model.notice = null
                dismissed = true
            },
        )
    }
}

@Composable
private fun StretchInstall(model: StretchModel, rev: Int, act: ((StretchModel) -> Unit) -> Unit) {
    Column(
        Modifier.fillMaxSize().padding(FmSpace.x5).verticalScroll(rememberScrollState()),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        model.library?.let { StretchFigureCanvas(it, null, 0.0, Modifier.size(width = 120.dp, height = 160.dp)) }
        Spacer(Modifier.height(FmSpace.x3))
        Text(StretchModel.INSTALL_TITLE, style = Fm.text.titleMedium, textAlign = TextAlign.Center)
        Spacer(Modifier.height(FmSpace.x3))
        Text(
            StretchModel.INSTALL_MESSAGE,
            style = Fm.text.bodyMedium.copy(color = Fm.colors.textSecondary),
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(FmSpace.x4))
        AccountFilledButton(StretchModel.INSTALL_BUTTON, "self_improvement", onClick = { act { it.install() } }, enabled = !model.busy)
        Spacer(Modifier.height(FmSpace.x2))
        StretchNotice(model, rev)
    }
}

@Composable
private fun StretchDashboard(
    model: StretchModel,
    rev: Int,
    act: ((StretchModel) -> Unit) -> Unit,
    onUninstall: () -> Unit,
    onDelete: (String) -> Unit,
) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    val haptics = FmHaptics.current
    var refreshing by remember { mutableStateOf(false) }
    FmPullToRefreshBox(
        isRefreshing = refreshing,
        onRefresh = {
            refreshing = true
            scope.launch {
                withContext(Dispatchers.IO) { model.load() }
                refreshing = false
                act { }
            }
        },
        modifier = Modifier.fillMaxSize(),
    ) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
        ) {
            FmCard {
                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                    for (pair in model.stats.chunked(2)) {
                        Row(Modifier.fillMaxWidth()) {
                            for (stat in pair) {
                                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                                    Text(
                                        stat.label.uppercase(),
                                        style = Fm.text.labelSmall.copy(fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = c.textSecondary),
                                    )
                                    Text(stat.value, style = Fm.text.bodyMedium)
                                }
                            }
                            if (pair.size == 1) Spacer(Modifier.weight(1f))
                        }
                    }
                }
            }
            FmSectionHeader(title = "Routine")
            for (routine in model.routines) {
                RoutineCard(routine, model.chosenRoutine?.id == routine.id) {
                    haptics.tap()
                    model.selectedRoutine = routine.id
                    act { }
                }
            }
            val chosen = model.chosenRoutine
            if (chosen != null) {
                AccountFilledButton(
                    "Start ${chosen.title.lowercase()}", "play_arrow",
                    onClick = {
                        haptics.commit()
                        act { it.start() }
                    },
                    modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                    enabled = chosen.steps.isNotEmpty() && !model.busy,
                    colors = ButtonDefaults.buttonColors(containerColor = c.success, contentColor = c.onAccent),
                )
                FmSectionHeader(title = "Steps", count = chosen.steps.size)
                FmCard {
                    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                        chosen.steps.forEachIndexed { index, step ->
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                                Text(
                                    (index + 1).toString(),
                                    modifier = Modifier.width(22.dp),
                                    textAlign = TextAlign.End,
                                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                                )
                                Text(step.title, modifier = Modifier.weight(1f), style = Fm.text.bodyMedium)
                                Text(StretchText.duration(step.seconds), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                            }
                        }
                    }
                }
            }
            StretchNotice(model, rev)
            FmCard {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text("Speak the cues", style = Fm.text.bodyLarge)
                        Text(
                            "Uses this phone's own text-to-speech voice.",
                            style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        )
                    }
                    FmSwitch(checked = model.voice, onCheckedChange = { on -> act { it.setVoice(on) } })
                }
            }
            val recent = model.recent
            if (recent.isNotEmpty()) {
                FmSectionHeader(title = "Recent sessions")
                for (row in recent) RecentRow(row) { onDelete(row.id) }
            }
            Text(StretchModel.DISCLAIMER, style = Fm.text.bodySmall.copy(color = c.textTertiary))
            Row(Modifier.fillMaxWidth()) {
                Spacer(Modifier.weight(1f))
                FmTextButton(onClick = onUninstall, contentColor = c.danger) { Text("Uninstall") }
            }
        }
    }
}

@Composable
private fun RoutineCard(routine: StretchRoutine, selected: Boolean, onSelect: () -> Unit) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.lg)
    Row(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(if (selected) c.accentSubtle else c.bgRaised)
            .border(if (selected) 2.dp else 1.dp, if (selected) c.accent else c.border, shape)
            .clickable(onClick = onSelect)
            .padding(FmSpace.x3),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        FmIcon(if (selected) "check_circle" else "radio_button_unchecked", size = 20.dp, tint = if (selected) c.accent else c.textTertiary)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(routine.title, modifier = Modifier.weight(1f), style = Fm.text.titleSmall)
                Text(routine.durationLabel, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            }
            Text(routine.summary, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
    }
}

@Composable
private fun RecentRow(row: StretchRecentRow, onRemove: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Box(Modifier.size(8.dp).clip(CircleShape).background(if (row.completed) c.success else c.warning))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text(row.title, style = Fm.text.bodyLarge)
                if (row.health) FmIcon("favorite", size = 11.dp, tint = c.danger, contentDescription = "In Apple Health")
            }
            Text(row.detail, style = Fm.text.bodySmall.copy(color = c.textSecondary))
            Text(row.whenText, style = Fm.text.bodySmall.copy(color = c.textTertiary))
        }
        FmIconButton(icon = "delete_outline", label = "Remove", onPressed = onRemove)
    }
}

@Composable
private fun StretchSession(
    model: StretchModel,
    rev: Int,
    run: StretchRun,
    library: StretchLibrary,
    act: ((StretchModel) -> Unit) -> Unit,
) {
    if (run.finished) StretchFinished(model, rev, run, library, act) else StretchActive(model, rev, run, library, act)
}

@Composable
private fun StretchActive(
    model: StretchModel,
    rev: Int,
    run: StretchRun,
    library: StretchLibrary,
    act: ((StretchModel) -> Unit) -> Unit,
) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    var frame by remember { mutableLongStateOf(0L) }
    LaunchedEffect(run.paused) {
        while (isActive && !run.paused) {
            androidx.compose.runtime.withFrameNanos { frame = it }
        }
    }
    val seconds = if (frame >= 0 && rev >= 0) model.figureSeconds(Instant.now()) else 0.0
    Column(Modifier.fillMaxSize().padding(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(run.routine.title, modifier = Modifier.weight(1f), style = Fm.text.titleSmall)
                Text(
                    "Step ${min(run.index + 1, run.steps.size)} of ${run.steps.size}",
                    style = Fm.text.bodySmall.copy(color = c.textSecondary),
                )
            }
            FmLinearProgressIndicator(
                modifier = Modifier.semantics { contentDescription = "Overall progress ${(run.progress * 100).roundToInt()} percent" },
                progress = { run.progress.toFloat() },
            )
        }
        Box(Modifier.fillMaxWidth().weight(1f).heightIn(min = 220.dp)) {
            StretchFigureCanvas(library, run.step, seconds, Modifier.fillMaxSize())
            Text(
                StretchText.countdown(run.stepRemaining),
                modifier = Modifier.align(Alignment.TopEnd).semantics { contentDescription = "${run.stepRemaining} seconds left in this step" },
                style = Fm.text.titleLarge.copy(
                    fontSize = 34.sp,
                    fontWeight = FontWeight.SemiBold,
                    fontFamily = FontFamily.Default,
                    fontFeatureSettings = "tnum",
                    color = if (run.paused) c.textTertiary else c.textPrimary,
                ),
            )
        }
        Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Text(run.step?.title ?: "", style = Fm.text.titleLarge)
            Text(run.step?.say ?: "", style = Fm.text.bodyMedium.copy(color = c.textSecondary))
            run.nextStep?.let {
                Text("Next: ${it.title}", modifier = Modifier.padding(top = FmSpace.x1), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
        }
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmIconButton(icon = "skip_previous", label = "Back", onPressed = { act { it.back() } })
            Spacer(Modifier.weight(1f))
            Box(
                Modifier
                    .size(72.dp)
                    .clip(CircleShape)
                    .background(c.accent)
                    .clickable {
                        haptics.tap()
                        act { it.togglePause() }
                    }
                    .semantics { contentDescription = if (run.paused) "Resume" else "Pause" },
                contentAlignment = Alignment.Center,
            ) {
                FmIcon(if (run.paused) "play_arrow" else "pause", size = 34.dp, tint = c.onAccent)
            }
            Spacer(Modifier.weight(1f))
            FmIconButton(icon = "skip_next", label = "Skip", onPressed = { act { it.skip() } })
            FmIconButton(
                icon = if (model.voice) "volume_up_outlined" else "volume_off_outlined",
                label = if (model.voice) "Turn the voice off" else "Turn the voice on",
                onPressed = { act { it.setVoice(!model.voice) } },
            )
            FmIconButton(
                icon = "stop_circle_outlined",
                label = "Stop",
                color = c.danger,
                onPressed = {
                    haptics.commit()
                    act { it.stop() }
                },
            )
        }
    }
}

@Composable
private fun StretchFinished(
    model: StretchModel,
    rev: Int,
    run: StretchRun,
    library: StretchLibrary,
    act: ((StretchModel) -> Unit) -> Unit,
) {
    val c = Fm.colors
    Column(
        Modifier.fillMaxSize().padding(FmSpace.x5).verticalScroll(rememberScrollState()),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3, Alignment.CenterVertically),
    ) {
        StretchFigureCanvas(library, null, 0.0, Modifier.size(width = 140.dp, height = 185.dp))
        Text(if (run.completed) "Well done" else "Session stopped", style = Fm.text.titleLarge)
        Text(
            "${run.routine.title} · ${StretchText.duration(run.loggedSeconds)} · ${run.stepsDone} of ${run.steps.size} steps",
            style = Fm.text.bodyMedium.copy(color = c.textSecondary),
            textAlign = TextAlign.Center,
        )
        if (model.busy) {
            FmCircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
        } else if (model.logNote.isNotEmpty()) {
            Text(model.logNote, style = Fm.text.bodyMedium, textAlign = TextAlign.Center)
        }
        StretchNotice(model, rev)
        AccountFilledButton(
            "Done", "check_circle",
            onClick = {
                model.close()
                act { it.load() }
            },
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
            enabled = !model.busy,
        )
    }
}

@Composable
private fun StretchFigureCanvas(library: StretchLibrary, step: StretchStep?, seconds: Double, modifier: Modifier = Modifier) {
    val ink = Fm.colors.accent
    val eyeColor = Fm.colors.bgBase
    Canvas(modifier) {
        val figure = library.figure
        if (figure.width <= 0 || figure.height <= 0) return@Canvas
        val figureWidth = figure.width.toFloat()
        val figureHeight = figure.height.toFloat()
        val scale = min(size.width / figureWidth, size.height / figureHeight)
        val dx = (size.width - figureWidth * scale) / 2
        val dy = (size.height - figureHeight * scale) / 2
        fun point(x: Double, y: Double) = Offset(dx + x.toFloat() * scale, dy + y.toFloat() * scale)
        val shape = StretchMath.body(library, step, seconds)
        for (segment in shape.segments) {
            drawLine(
                ink,
                point(segment.from.x, segment.from.y),
                point(segment.to.x, segment.to.y),
                strokeWidth = segment.width.toFloat() * scale,
                cap = StrokeCap.Round,
            )
        }
        drawCircle(ink, radius = shape.headRadius.toFloat() * scale, center = point(shape.head.x, shape.head.y))
        for (spot in shape.eyes) {
            drawCircle(eyeColor, radius = shape.eyeRadius.toFloat() * scale, center = point(spot.x, spot.y))
        }
    }
}
