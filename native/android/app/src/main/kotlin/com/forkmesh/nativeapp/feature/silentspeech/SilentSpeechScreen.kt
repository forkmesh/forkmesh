package com.forkmesh.nativeapp.feature.silentspeech

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.silentspeech.SilentChannel
import com.forkmesh.nativeapp.silentspeech.SilentEvaluation
import com.forkmesh.nativeapp.silentspeech.SilentEvaluator
import com.forkmesh.nativeapp.silentspeech.SilentMark
import com.forkmesh.nativeapp.silentspeech.SilentNoiseFloor
import com.forkmesh.nativeapp.silentspeech.SilentReading
import com.forkmesh.nativeapp.silentspeech.SilentScope
import com.forkmesh.nativeapp.silentspeech.SilentSpectrum
import com.forkmesh.nativeapp.silentspeech.SilentText
import com.forkmesh.nativeapp.silentspeech.SilentVector
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

class SilentSpeechScreen : Screen() {
    override val title: String get() = "Silent Speech"

    private var entry by mutableStateOf("")
    private var showWords by mutableStateOf(false)

    @Composable
    override fun Content(navigator: Navigator) {
        val listener = SilentSpeechListener
        DisposableEffect(Unit) {
            listener.start()
            onDispose { listener.stop() }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar("Silent Speech", navigator)
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x3),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                MotionCard()
                listener.problem?.let { problem ->
                    FmListRow(problem, leading = { FmIcon("error", tint = Fm.colors.danger) }, showChevron = false)
                }
                TeachSection()
                WordsSection()
                CheckSection()
                DecodeSection()
            }
        }
    }

    @Composable
    private fun TeachSection() {
        val listener = SilentSpeechListener
        val reading = listener.reading
        if (reading == null) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                FmToneButton(
                    "Read sentences · 5 min",
                    if (listener.connected) { { listener.startReading() } } else null,
                    icon = "play_arrow",
                    tone = FmTone.Accent,
                    filled = true,
                    height = 40.dp,
                )
                Text(
                    listener.readingSummary ?: "Mouth each word, short stop between words.",
                    style = Fm.text.bodySmall,
                    color = Fm.colors.textSecondary,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            return
        }
        FmCard(color = Fm.colors.bgRaised, padding = PaddingValues(FmSpace.x3)) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text(sentenceText(reading), style = Fm.text.titleLarge, color = Fm.colors.textPrimary, modifier = Modifier.fillMaxWidth())
                Text(if (listener.calibrating) "Keep still for two seconds…" else readingDetail(reading), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    FmToneButton("Again", if (reading.position > 0) { { listener.readAgain() } } else null, icon = "refresh")
                    FmToneButton("Skip", { listener.skipSentence() }, icon = "skip_next")
                    if (listener.lastSentence.isNotEmpty()) FmToneButton("Drop last", { listener.dropLastSentence() }, icon = "undo")
                    Spacer(Modifier.weight(1f))
                    FmToneButton("Finish", { listener.finishReading() }, icon = "stop", tone = FmTone.Accent)
                }
            }
        }
    }

    @Composable
    private fun sentenceText(reading: SilentReading) = buildAnnotatedString {
        reading.sentence.forEachIndexed { i, word ->
            if (i > 0) append(" ")
            val style = when {
                i < reading.position -> SpanStyle(color = Fm.colors.textTertiary)
                i == reading.position -> SpanStyle(color = Fm.colors.accent, fontWeight = FontWeight.Bold)
                else -> SpanStyle(color = Fm.colors.textPrimary)
            }
            withStyle(style) { append(word) }
        }
    }

    private fun readingDetail(reading: SilentReading): String {
        val saved = if (reading.collected == 1) "1 take" else "${reading.collected} takes"
        return "Sentence ${reading.sentencesRead + 1} · ${reading.clock} · $saved"
    }

    @OptIn(ExperimentalLayoutApi::class)
    @Composable
    private fun WordsSection() {
        val listener = SilentSpeechListener
        val dataset = listener.dataset
        val counts = dataset.counts()
        val taught = dataset.words.count { (counts[it.word] ?: 0) > 0 }
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.sm)).clickable(role = Role.Button) { showWords = !showWords },
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
            ) {
                Text("Words", style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                Text("$taught of ${dataset.words.size} taught · ${dataset.takes.size} takes", style = Fm.text.bodySmall, color = Fm.colors.textSecondary, modifier = Modifier.weight(1f))
                FmIcon(if (showWords) "expand_less" else "expand_more", size = 20.dp, tint = Fm.colors.textSecondary)
            }
            if (!showWords) return@Column
            FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                for (word in dataset.words) {
                    Chip("${word.word} ${counts[word.word] ?: 0}", listener.selectedWord == word.word) { listener.selectedWord = word.word }
                }
            }
            listener.selectedWord?.let { word ->
                val active = listener.hold == SilentHold.Train(word)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    HoldButton(
                        if (active) "Mouth “$word”, let go" else "Hold, mouth “$word”",
                        active = active,
                        enabled = listener.connected && listener.reading == null,
                        height = 44,
                        modifier = Modifier.weight(1f),
                        onBegin = { listener.beginHold(SilentHold.Train(word)) },
                        onEnd = { listener.endHold() },
                    )
                    FmIconButton("undo", "Delete the last take of $word", if ((counts[word] ?: 0) > 0) { { listener.undoTake(word) } } else null, color = Fm.colors.textSecondary, iconSize = 20.dp)
                    FmIconButton("delete_outline", "Remove $word and its takes", { listener.removeWord(word) }, color = Fm.colors.textSecondary, iconSize = 20.dp)
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmTextField(
                    entry,
                    { entry = it },
                    Modifier.weight(1f),
                    hint = "Add a word or your own sentence",
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    keyboardActions = KeyboardActions(onDone = { addEntry() }),
                )
                FmToneButton("Add", if (SilentText.words(entry).isEmpty()) null else { { addEntry() } }, icon = "add", height = 40.dp)
            }
        }
    }

    private fun addEntry() {
        if (SilentSpeechListener.addEntry(entry)) entry = ""
    }

    @Composable
    private fun CheckSection() {
        val listener = SilentSpeechListener
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                for (channel in SilentChannel.entries) {
                    if (channel == SilentChannel.ACCELERATION && !listener.hasAcceleration) continue
                    val on = channel in listener.channels
                    Chip(channel.label, on) { listener.toggleChannel(channel, !on) }
                }
                Spacer(Modifier.weight(1f))
                FmToneButton(
                    if (listener.evaluating) "Checking…" else "Check",
                    if (SilentEvaluator.requirement(listener.dataset) == null) { { listener.evaluate() } } else null,
                    icon = "check_circle",
                    busy = listener.evaluating,
                )
            }
            listener.evaluation?.let { result ->
                Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                    ConfusionMap(result)
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(result.headline, style = Fm.text.labelMedium, color = Fm.colors.textPrimary)
                        Text(result.verdict, style = Fm.text.bodySmall, color = if (result.significant) Fm.colors.success else Fm.colors.textSecondary)
                        for ((word, guess, count) in result.mixups()) {
                            Text("$word → $guess ×$count", style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun DecodeSection() {
        val listener = SilentSpeechListener
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            val active = listener.hold == SilentHold.Decode
            HoldButton(
                if (active) "Mouth words, then let go" else "Hold and mouth a word",
                active = active,
                enabled = listener.connected && listener.dataset.takes.isNotEmpty() && listener.reading == null,
                height = 52,
                onBegin = { listener.beginHold(SilentHold.Decode) },
                onEnd = { listener.endHold() },
            )
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                Chip("Hands-free", listener.autoDetect) { listener.updateAutoDetect(!listener.autoDetect) }
                Chip("Speak", listener.speakAloud) { listener.updateSpeakAloud(!listener.speakAloud) }
                val suggested = listener.evaluation?.suggestedMaxDistance
                Chip("Skip far-off", listener.maxDistance != null, enabled = listener.maxDistance != null || suggested != null) {
                    listener.updateMaxDistance(if (listener.maxDistance != null) null else suggested)
                }
                Spacer(Modifier.weight(1f))
                if (listener.autoDetect) {
                    FmIconButton(
                        "refresh",
                        "Measure stillness again",
                        if (listener.calibrating || !listener.connected) null else { { listener.calibrate() } },
                        color = Fm.colors.textSecondary,
                        iconSize = 20.dp,
                    )
                }
            }
            val match = listener.lastMatch
            Text(
                statusLine(),
                style = Fm.text.bodySmall,
                color = when {
                    match == null -> Fm.colors.textSecondary
                    match.accepted -> Fm.colors.textPrimary
                    else -> Fm.colors.warning
                },
            )
            val text = listener.transcript.text
            val has = listener.transcript.words.isNotEmpty()
            FmCard(color = Fm.colors.bgRaised, padding = PaddingValues(start = FmSpace.x3, end = FmSpace.x1, top = FmSpace.x2)) {
                Column {
                    SelectionContainer {
                        Text(
                            text.ifEmpty { "Words you mouth appear here." },
                            style = Fm.text.bodyLarge,
                            color = if (text.isEmpty()) Fm.colors.textTertiary else Fm.colors.textPrimary,
                            modifier = Modifier.fillMaxWidth().heightIn(min = 40.dp),
                        )
                    }
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        val tint = if (has) Fm.colors.textSecondary else Fm.colors.textDisabled
                        FmIconButton("undo", "Undo", if (has) { { listener.undoWord() } } else null, color = tint, iconSize = 20.dp)
                        FmIconButton("content_copy", "Copy", if (has) { { listener.copyTranscript() } } else null, color = tint, iconSize = 20.dp)
                        FmIconButton(
                            "volume_up_outlined",
                            "Say it",
                            if (has) { { listener.speak(listener.transcript.words.joinToString(" ") { listener.dataset.phrase(it) }) } } else null,
                            color = tint,
                            iconSize = 20.dp,
                        )
                        FmIconButton("close", "Clear", if (has) { { listener.clearTranscript() } } else null, color = tint, iconSize = 20.dp)
                    }
                }
            }
        }
    }

    private fun statusLine(): String {
        val listener = SilentSpeechListener
        listener.lastMatch?.let { return it.summary }
        return when {
            !listener.autoDetect -> "Hold for each word, or turn on Hands-free."
            listener.calibrating -> "Keep still for two seconds…"
            listener.noise == null -> "Waiting to measure stillness."
            listener.detecting -> "Hearing a word…"
            else -> "Listening. Keep still between words."
        }
    }
}

@Composable
private fun Chip(label: String, on: Boolean, enabled: Boolean = true, onTap: () -> Unit) {
    val shape = RoundedCornerShape(FmRadius.full)
    Box(
        Modifier
            .height(30.dp)
            .clip(shape)
            .background(if (on) Fm.colors.accentSubtle else Fm.colors.bgRaised)
            .border(1.dp, if (on) Fm.colors.accent.copy(alpha = 0.5f) else Fm.colors.border, shape)
            .clickable(enabled = enabled, role = Role.Switch, onClick = onTap)
            .semantics { selected = on }
            .alpha(if (enabled) 1f else 0.45f)
            .padding(horizontal = 10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, style = Fm.text.labelMedium, color = if (on) Fm.colors.accent else Fm.colors.textSecondary, maxLines = 1)
    }
}

@Composable
private fun HoldButton(label: String, active: Boolean, enabled: Boolean, height: Int, modifier: Modifier = Modifier.fillMaxWidth(), onBegin: () -> Unit, onEnd: () -> Unit) {
    val shape = RoundedCornerShape(FmRadius.lg)
    Box(
        modifier
            .heightIn(min = height.dp)
            .clip(shape)
            .background(if (active) Fm.colors.accent else Fm.colors.accentSubtle)
            .pointerInput(enabled) {
                if (!enabled) return@pointerInput
                detectTapGestures(onPress = {
                    onBegin()
                    tryAwaitRelease()
                    onEnd()
                })
            }
            .semantics {
                role = Role.Button
                contentDescription = label
                stateDescription = if (active) "Recording" else "Ready"
                if (enabled) onClick(label = if (active) "Stop" else "Start") {
                    if (active) onEnd() else onBegin()
                    true
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            label,
            style = Fm.text.labelLarge,
            color = if (active) Fm.colors.onAccent else if (enabled) Fm.colors.accent else Fm.colors.textDisabled,
            textAlign = TextAlign.Center,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = FmSpace.x3),
        )
    }
}

private val AXIS_NAMES = listOf("X nod", "Y tilt", "Z turn")

@Composable
private fun axisColors(): List<Color> =
    if (Fm.colors.dark) listOf(Color(0xFF3987E5), Color(0xFFD95926), Color(0xFF199E70))
    else listOf(Color(0xFF2A78D6), Color(0xFFEB6834), Color(0xFF1BAF7A))

private fun formatAxis(value: Double, unit: String): String =
    if (unit == "g") String.format(Locale.US, "%.2f", value) else "${value.roundToInt()}"

private fun timeX(t: Double, end: Double, width: Float): Float = ((t - (end - SilentScope.WINDOW)) / SilentScope.WINDOW).toFloat() * width

private fun DrawScope.shade(marks: List<SilentMark>, end: Double, color: Color, label: Color?, measurer: TextMeasurer?) {
    for (mark in marks) {
        val left = max(0f, timeX(mark.start, end, size.width))
        val right = min(size.width, timeX(mark.end, end, size.width))
        if (right <= left) continue
        drawRect(color, Offset(left, 0f), Size(right - left, size.height))
        if (label != null && measurer != null && mark.label.isNotEmpty()) {
            val layout = measurer.measure(mark.label, TextStyle(fontSize = 9.sp, fontWeight = FontWeight.SemiBold, color = label))
            drawText(layout, topLeft = Offset((left + right) / 2 - layout.size.width / 2, 0f))
        }
    }
}

@Composable
private fun MotionCard() {
    val listener = SilentSpeechListener
    val scope = listener.scope
    val frames = scope.frames
    val end = scope.latest ?: 0.0
    val times = frames.map { it.t }
    FmCard(color = Fm.colors.bgRaised, padding = PaddingValues(FmSpace.x3), radius = FmRadius.lg) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmIcon("headphones", size = 22.dp, tint = if (listener.connected) Fm.colors.success else Fm.colors.textTertiary)
                Column(Modifier.weight(1f)) {
                    Text(if (listener.connected) "Reading your earbuds" else "Put in your earbuds", style = Fm.text.titleSmall)
                    Text(statusDetail(), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                }
                FmIconButton("refresh", "Zero the head angle", if (listener.connected) { { listener.recentre() } } else null, color = Fm.colors.textSecondary, iconSize = 20.dp)
            }
            AxesStrip("Turning", "°/s", 30.0, frames.map { it.rotation }, times, end, scope.marks)
            AxesStrip("Head angle", "°", 5.0, frames.map { it.angle }, times, end, scope.marks)
            if (listener.hasAcceleration) AxesStrip("Movement", "g", 0.05, frames.map { it.acceleration ?: SilentVector.ZERO }, times, end, scope.marks)
            Row(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                EnergyStrip(frames.map { it.energy }, times, end, scope.marks, listener.noise, listener.hold != null || listener.detecting, Modifier.weight(1f))
                SpectrumView(SilentScope.spectrum(frames.map { it.rotation }), Modifier.weight(1f))
            }
        }
    }
}

private fun statusDetail(): String {
    val listener = SilentSpeechListener
    if (!listener.supported) return "Head tracking needs Android 13 or later"
    if (!listener.connected) return "Earbuds with head tracking, connected to this phone"
    return listener.rate?.let { "${it.roundToInt()} Hz · last ${SilentScope.WINDOW.toInt()} seconds" } ?: "Waiting for motion"
}

@Composable
private fun AxesStrip(title: String, unit: String, floor: Double, values: List<SilentVector>, times: List<Double>, end: Double, marks: List<SilentMark>) {
    val colors = axisColors()
    val latest = values.lastOrNull() ?: SilentVector.ZERO
    val parts = listOf(latest.x, latest.y, latest.z)
    val scale = max(floor, values.maxOfOrNull { maxOf(abs(it.x), abs(it.y), abs(it.z)) } ?: 0.0)
    val shadeColor = Fm.colors.accent.copy(alpha = 0.10f)
    val zero = Fm.colors.border
    val measurer = rememberTextMeasurer()
    val scaleStyle = TextStyle(fontSize = 9.sp, color = Fm.colors.textTertiary)
    val description = "$title: " + parts.indices.joinToString(", ") { "${AXIS_NAMES[it]} ${formatAxis(parts[it], unit)}" } + " $unit"
    Column(Modifier.clearAndSetSemantics { contentDescription = description }, verticalArrangement = Arrangement.spacedBy(1.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Text("$title $unit", style = Fm.text.labelSmall, color = Fm.colors.textSecondary, modifier = Modifier.weight(1f))
            for (axis in 0 until 3) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(3.dp)) {
                    Box(Modifier.size(6.dp).clip(CircleShape).background(colors[axis]))
                    Text("${AXIS_NAMES[axis]} ${formatAxis(parts[axis], unit)}", style = Fm.text.labelSmall.copy(fontFeatureSettings = "tnum"), color = Fm.colors.textPrimary)
                }
            }
        }
        Canvas(Modifier.fillMaxWidth().height(34.dp)) {
            shade(marks, end, shadeColor, null, null)
            val mid = size.height / 2
            drawLine(zero, Offset(0f, mid), Offset(size.width, mid), strokeWidth = 1.dp.toPx())
            for (axis in 0 until 3) {
                val path = Path()
                values.forEachIndexed { i, v ->
                    val value = if (axis == 0) v.x else if (axis == 1) v.y else v.z
                    val x = timeX(times[i], end, size.width)
                    val y = mid - (value / scale).toFloat() * (mid - 2.dp.toPx())
                    if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
                }
                drawPath(path, colors[axis], style = Stroke(width = 1.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
            }
            val layout = measurer.measure("±${formatAxis(scale, unit)}", scaleStyle)
            drawText(layout, topLeft = Offset(size.width - layout.size.width - 2.dp.toPx(), 0f))
        }
    }
}

@Composable
private fun EnergyStrip(energies: List<Double>, times: List<Double>, end: Double, marks: List<SilentMark>, noise: SilentNoiseFloor?, active: Boolean, modifier: Modifier) {
    val top = max(noise?.let { it.on * 2 } ?: 0.5, energies.maxOrNull() ?: 0.0)
    val shadeColor = Fm.colors.accent.copy(alpha = 0.10f)
    val labelColor = Fm.colors.textSecondary
    val line = Fm.colors.textTertiary
    val fill = (if (active) Fm.colors.accent else Fm.colors.textTertiary).copy(alpha = 0.35f)
    val measurer = rememberTextMeasurer()
    Column(modifier.clearAndSetSemantics { contentDescription = if (active) "Motion energy, hearing a word" else "Motion energy" }, verticalArrangement = Arrangement.spacedBy(1.dp)) {
        Text(if (active) "Energy · hearing" else "Energy · words heard", style = Fm.text.labelSmall, color = if (active) Fm.colors.accent else Fm.colors.textSecondary)
        Canvas(Modifier.fillMaxWidth().height(40.dp)) {
            shade(marks, end, shadeColor, labelColor, measurer)
            fun y(value: Double) = size.height - (min(value / top, 1.0)).toFloat() * (size.height - 2.dp.toPx())
            noise?.let { floor ->
                for ((level, alpha) in listOf(floor.on to 0.8f, floor.off to 0.4f)) {
                    drawLine(line.copy(alpha = line.alpha * alpha), Offset(0f, y(level)), Offset(size.width, y(level)), strokeWidth = 1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(3.dp.toPx(), 3.dp.toPx())))
                }
            }
            if (energies.isNotEmpty()) {
                val area = Path()
                area.moveTo(timeX(times.first(), end, size.width), size.height)
                energies.forEachIndexed { i, e -> area.lineTo(timeX(times[i], end, size.width), y(e)) }
                area.lineTo(timeX(times.last(), end, size.width), size.height)
                area.close()
                drawPath(area, fill)
            }
        }
    }
}

@Composable
private fun SpectrumView(spectrum: SilentSpectrum, modifier: Modifier) {
    val peak = spectrum.peakHz?.let { String.format(Locale.US, "%.1f Hz", it) } ?: "–"
    val accent = Fm.colors.accent
    val measurer = rememberTextMeasurer()
    val tickStyle = TextStyle(fontSize = 8.sp, color = Fm.colors.textTertiary)
    Column(modifier.clearAndSetSemantics { contentDescription = "Head motion frequency, peak $peak" }, verticalArrangement = Arrangement.spacedBy(1.dp)) {
        Text("Frequency · peak $peak", style = Fm.text.labelSmall.copy(fontFeatureSettings = "tnum"), color = Fm.colors.textSecondary)
        Canvas(Modifier.fillMaxWidth().height(40.dp)) {
            val bins = spectrum.power.drop(1)
            val topPower = max(bins.maxOrNull() ?: 0.0, 1.0)
            val label = 10.dp.toPx()
            val height = size.height - label
            if (bins.isNotEmpty()) {
                val step = size.width / bins.size
                bins.forEachIndexed { k, p ->
                    val h = max(1f, (p / topPower).toFloat() * height)
                    val hz = (k + 1) * spectrum.binHz
                    val isPeak = spectrum.peakHz?.let { abs(it - hz) < 1e-9 } ?: false
                    drawRoundRect(
                        if (isPeak) accent else accent.copy(alpha = 0.45f),
                        Offset(k * step + 0.5f, height - h),
                        Size(max(1f, step - 1f), h),
                        CornerRadius(1f, 1f),
                    )
                }
            }
            val span = spectrum.binHz * max(spectrum.power.size - 1, 1)
            for (hz in listOf(0.0, 5.0, 10.0)) {
                val layout = measurer.measure(if (hz == 10.0) "10 Hz" else "${hz.toInt()}", tickStyle)
                val x = (hz / span).toFloat() * size.width
                val left = when (hz) {
                    0.0 -> 0f
                    10.0 -> min(x, size.width) - layout.size.width
                    else -> x - layout.size.width / 2
                }
                drawText(layout, topLeft = Offset(left, size.height - layout.size.height))
            }
        }
    }
}

@Composable
private fun ConfusionMap(evaluation: SilentEvaluation) {
    val accent = Fm.colors.accent
    val warning = Fm.colors.warning
    val empty = Fm.colors.bgOverlay
    Canvas(Modifier.size(96.dp).semantics { contentDescription = "Mix-up map: rows are the words you mouthed, columns what each was taken for" }) {
        val n = evaluation.words.size
        if (n == 0) return@Canvas
        val cell = min(size.width, size.height) / n
        val inset = if (n > 24) 0f else 0.5f
        for (row in 0 until n) {
            val total = evaluation.confusion[row].sum()
            for (column in 0 until n) {
                val count = evaluation.confusion[row][column]
                val share = if (total > 0) count.toFloat() / total else 0f
                val color = when {
                    count == 0 -> empty
                    row == column -> accent.copy(alpha = 0.25f + 0.75f * share)
                    else -> warning.copy(alpha = 0.25f + 0.75f * share)
                }
                drawRect(color, Offset(column * cell + inset, row * cell + inset), Size(cell - 2 * inset, cell - 2 * inset))
            }
        }
    }
}
