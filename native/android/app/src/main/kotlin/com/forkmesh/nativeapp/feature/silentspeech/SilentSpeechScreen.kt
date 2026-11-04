package com.forkmesh.nativeapp.feature.silentspeech

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.silentspeech.SilentChannel
import com.forkmesh.nativeapp.silentspeech.SilentDataset
import com.forkmesh.nativeapp.silentspeech.SilentEvaluation
import com.forkmesh.nativeapp.silentspeech.SilentEvaluator
import com.forkmesh.nativeapp.silentspeech.SilentReading
import com.forkmesh.nativeapp.silentspeech.SilentWord
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace

class SilentSpeechScreen : Screen() {
    override val title: String get() = "Silent Speech"

    private var newWord by mutableStateOf("")
    private var newPhrase by mutableStateOf("")

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
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(FmSpace.x4),
                verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
            ) {
                StatusCard()
                listener.problem?.let { problem ->
                    FmListRow(problem, leading = { FmIcon("error", tint = Fm.colors.danger) }, showChevron = false)
                }
                WordsSection()
                TrainSection()
                CheckSection()
                DecodeSection()
                Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
                    FmSectionHeader("How it works", icon = "info_outline")
                    Note(
                        "Earbuds with head tracking, such as Pixel Buds Pro or Galaxy Buds with 360 Audio, report how your head turns. When you mouth a word, " +
                            "your jaw and face move your head a little, and each word moves it its own way. Silent Speech keeps your takes of each word and matches " +
                            "a new movement to the closest ones, letting for tempo, so it learns your words, not anyone else's.",
                    )
                    Note("It is an experiment with a small vocabulary. Reseating the earbuds can change the signal; record a few fresh takes if it starts missing. Motion data stays on this phone.")
                }
                Spacer(Modifier.height(FmSpace.x4))
            }
        }
    }

    @Composable
    private fun StatusCard() {
        val listener = SilentSpeechListener
        FmCard(color = Fm.colors.bgRaised, padding = PaddingValues(FmSpace.x3)) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FmIcon("headphones", size = 28.dp, tint = if (listener.connected) Fm.colors.success else Fm.colors.textTertiary)
                    Spacer(Modifier.width(FmSpace.x3))
                    Column(Modifier.weight(1f)) {
                        Text(if (listener.connected) "Reading your earbuds" else "Put in your earbuds", style = Fm.text.titleMedium)
                        Text(statusDetail(), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
                    }
                }
                LevelBar(listener.level, listener.hold != null || listener.detecting)
            }
        }
    }

    private fun statusDetail(): String {
        val listener = SilentSpeechListener
        if (!listener.supported) return "Head tracking needs Android 13 or later"
        if (!listener.connected) return "Needs earbuds with head tracking connected to this phone"
        return listener.rate?.let { "Head motion at ${Math.round(it)} Hz" } ?: "Waiting for motion"
    }

    @Composable
    private fun WordsSection() {
        val listener = SilentSpeechListener
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmSectionHeader("Words", icon = "tune", count = listener.dataset.words.size)
            for (word in listener.dataset.words) {
                val selected = listener.selectedWord == word.word
                FmListRow(
                    word.word,
                    subtitle = wordDetail(listener.dataset, word),
                    leading = { FmIcon(if (selected) "check_circle" else "touch_app", tint = if (selected) Fm.colors.accent else Fm.colors.textTertiary) },
                    trailing = {
                        Row {
                            if (listener.dataset.count(word.word) > 0) {
                                FmIconButton("undo", "Delete the last take of ${word.word}", { listener.undoTake(word.word) }, color = Fm.colors.textSecondary, iconSize = 20.dp)
                            }
                            FmIconButton("delete_outline", "Remove ${word.word} and its takes", { listener.removeWord(word.word) }, color = Fm.colors.textSecondary, iconSize = 20.dp)
                        }
                    },
                    onTap = { listener.selectedWord = word.word },
                    showChevron = false,
                )
            }
            FmTextField(newWord, { newWord = it }, Modifier.fillMaxWidth(), hint = "New word")
            FmTextField(newPhrase, { newPhrase = it }, Modifier.fillMaxWidth(), hint = "Say instead (optional), for example: Could I have some water?")
            FmToneButton(
                "Add word",
                if (SilentDataset.clean(newWord).isEmpty()) null else {
                    {
                        if (listener.addWord(newWord, newPhrase)) {
                            newWord = ""
                            newPhrase = ""
                        }
                    }
                },
                icon = "add",
            )
        }
    }

    private fun wordDetail(dataset: SilentDataset, word: SilentWord): String {
        val takes = dataset.count(word.word)
        val count = if (takes == 1) "1 take" else "$takes takes"
        return if (word.phrase.isEmpty()) count else "$count · says “${word.phrase}”"
    }

    @Composable
    private fun TrainSection() {
        val listener = SilentSpeechListener
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmSectionHeader("Teach", icon = "touch_app")
            ReadSection()
            val word = listener.selectedWord
            if (word == null) {
                Note("Add a word to teach it.")
                return@Column
            }
            val active = listener.hold == SilentHold.Train(word)
            HoldButton(
                if (active) "Mouth “$word”, then let go" else "Hold and mouth “$word”",
                active = active,
                enabled = listener.connected && listener.reading == null,
                onBegin = { listener.beginHold(SilentHold.Train(word)) },
                onEnd = { listener.endHold() },
            )
            Note("Or teach one word at a time: keep your head still, hold the button, mouth the word without a sound, then let go.")
        }
    }

    @Composable
    private fun ReadSection() {
        val listener = SilentSpeechListener
        val reading = listener.reading
        if (reading == null) {
            FmToneButton(
                "Read sentences for 5 minutes",
                if (listener.connected && listener.dataset.words.size >= 2) { { listener.startReading() } } else null,
                icon = "play_arrow",
            )
            Note(
                listener.readingSummary?.let { "$it Check my words to see how well it tells them apart." }
                    ?: "The best way to teach it: read short sentences made of your words for five minutes. Each word you mouth is saved as a take, so every word gets dozens of takes.",
            )
            return
        }
        FmCard(color = Fm.colors.bgRaised, padding = PaddingValues(FmSpace.x3)) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                Text(sentenceText(reading), style = Fm.text.titleLarge, color = Fm.colors.textPrimary, modifier = Modifier.fillMaxWidth())
                Text(readingDetail(reading), style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
            }
        }
        Note(
            if (listener.calibrating) "Keep your head still for two seconds while it measures how still you are."
            else "Mouth each word without a sound, with a short stop between words. The highlight moves on as each word is heard.",
        )
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            FmToneButton("Start again", if (reading.position > 0) { { listener.readAgain() } } else null, icon = "refresh")
            FmToneButton("Skip", { listener.skipSentence() }, icon = "skip_next")
            if (listener.lastSentence.isNotEmpty()) FmToneButton("Drop last sentence", { listener.dropLastSentence() }, icon = "undo")
            FmToneButton("Finish", { listener.finishReading() }, icon = "stop")
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
        val saved = if (reading.collected == 1) "1 take saved" else "${reading.collected} takes saved"
        return "Sentence ${reading.sentencesRead + 1} · ${reading.clock} · $saved"
    }

    @Composable
    private fun CheckSection() {
        val listener = SilentSpeechListener
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmSectionHeader("Check", icon = "check_circle")
            Note("Tests every take against all the others, then against 500 shuffles of the labels, to show whether your head motion really tells the words apart.")
            for (channel in SilentChannel.entries) {
                if (channel == SilentChannel.ACCELERATION && !listener.hasAcceleration) continue
                SwitchRow(channel.label, channel.detail, channel in listener.channels) { listener.toggleChannel(channel, it) }
            }
            val requirement = SilentEvaluator.requirement(listener.dataset)
            if (requirement != null) {
                Note(requirement)
            } else {
                FmToneButton(
                    if (listener.evaluating) "Checking…" else "Check my words",
                    { listener.evaluate() },
                    icon = "check_circle",
                    busy = listener.evaluating,
                )
            }
            listener.evaluation?.let { result ->
                Text(result.headline, style = Fm.text.titleSmall, color = Fm.colors.textPrimary)
                Text(result.verdict, style = Fm.text.bodySmall, color = if (result.significant) Fm.colors.success else Fm.colors.textSecondary)
                ConfusionGrid(result)
            }
        }
    }

    @Composable
    private fun DecodeSection() {
        val listener = SilentSpeechListener
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
            FmSectionHeader("Speak silently", icon = "hearing")
            val active = listener.hold == SilentHold.Decode
            HoldButton(
                if (active) "Mouth a word, then let go" else "Hold and mouth a word",
                active = active,
                enabled = listener.connected && listener.dataset.takes.isNotEmpty() && listener.reading == null,
                onBegin = { listener.beginHold(SilentHold.Decode) },
                onEnd = { listener.endHold() },
            )
            SwitchRow("Listen without holding", autoDetail(), listener.autoDetect) { listener.updateAutoDetect(it) }
            if (listener.autoDetect) {
                FmToneButton(
                    if (listener.calibrating) "Keep still…" else "Measure stillness again",
                    if (listener.calibrating || !listener.connected) null else { { listener.calibrate() } },
                    icon = "refresh",
                )
            }
            SwitchRow("Say each word aloud", null, listener.speakAloud) { listener.updateSpeakAloud(it) }
            val suggested = listener.evaluation?.suggestedMaxDistance
            SwitchRow(
                "Skip far-off matches",
                "Uses the limit found by Check my words, so a cough or a nod is not written down as a word.",
                listener.maxDistance != null,
                enabled = listener.maxDistance != null || suggested != null,
            ) { on -> listener.updateMaxDistance(if (on) suggested ?: listener.maxDistance else null) }
            listener.lastMatch?.let { match ->
                Text(match.summary, style = Fm.text.bodyMedium, color = if (match.accepted) Fm.colors.textPrimary else Fm.colors.warning)
            }
            val text = listener.transcript.text
            FmCard(color = Fm.colors.bgRaised, padding = PaddingValues(FmSpace.x3)) {
                SelectionContainer {
                    Text(
                        text.ifEmpty { "Words you mouth appear here." },
                        style = Fm.text.bodyLarge,
                        color = if (text.isEmpty()) Fm.colors.textTertiary else Fm.colors.textPrimary,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp),
                    )
                }
            }
            val has = listener.transcript.words.isNotEmpty()
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                FmToneButton("Undo", if (has) { { listener.undoWord() } } else null, icon = "undo")
                FmToneButton("Copy", if (has) { { listener.copyTranscript() } } else null, icon = "content_copy")
                FmToneButton(
                    "Say it",
                    if (has) { { listener.speak(listener.transcript.words.joinToString(" ") { listener.dataset.phrase(it) }) } } else null,
                    icon = "volume_up_outlined",
                )
                FmToneButton("Clear", if (has) { { listener.clearTranscript() } } else null, icon = "close")
            }
        }
    }

    private fun autoDetail(): String {
        val listener = SilentSpeechListener
        return when {
            !listener.autoDetect -> "Hold the button for each word, or turn this on to pick out words by themselves."
            listener.calibrating -> "Keep your head still for two seconds while it measures how still you are."
            listener.noise == null -> "Waiting for your earbuds to measure stillness."
            listener.detecting -> "Hearing a word…"
            else -> "Listening for words. Keep your head still between them."
        }
    }
}

@Composable
private fun Note(text: String) {
    Text(text, style = Fm.text.bodySmall, color = Fm.colors.textSecondary)
}

@Composable
private fun SwitchRow(title: String, detail: String?, checked: Boolean, enabled: Boolean = true, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
            detail?.let { Text(it, style = Fm.text.bodySmall, color = Fm.colors.textSecondary) }
        }
        Spacer(Modifier.width(FmSpace.x2))
        FmSwitch(checked = checked, onCheckedChange = onChange, enabled = enabled)
    }
}

@Composable
private fun HoldButton(label: String, active: Boolean, enabled: Boolean, onBegin: () -> Unit, onEnd: () -> Unit) {
    val shape = RoundedCornerShape(FmRadius.lg)
    Box(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp)
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
            modifier = Modifier.padding(FmSpace.x3),
        )
    }
}

@Composable
private fun LevelBar(level: Double, active: Boolean) {
    val fraction = (level / 2).coerceIn(0.0, 1.0).toFloat()
    Box(
        Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(FmRadius.full)).background(Fm.colors.border)
            .semantics { contentDescription = "Motion level ${Math.round(fraction * 100)} percent" },
    ) {
        Box(Modifier.fillMaxHeight().fillMaxWidth(fraction).background(if (active) Fm.colors.accent else Fm.colors.textTertiary))
    }
}

@Composable
private fun ConfusionGrid(evaluation: SilentEvaluation) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Note("Rows: the word you mouthed. Columns: what it was taken for.")
        Column(Modifier.horizontalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                Spacer(Modifier.width(64.dp))
                for (word in evaluation.words) {
                    Text(word, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.width(44.dp), textAlign = TextAlign.Center)
                }
            }
            evaluation.words.forEachIndexed { row, word ->
                Row(horizontalArrangement = Arrangement.spacedBy(2.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(word, style = Fm.text.labelSmall, color = Fm.colors.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.width(64.dp))
                    for (column in evaluation.words.indices) {
                        val count = evaluation.confusion[row][column]
                        val diagonal = row == column && count > 0
                        Box(
                            Modifier.size(width = 44.dp, height = 28.dp).clip(RoundedCornerShape(FmRadius.sm))
                                .background(if (diagonal) Fm.colors.accent else Fm.colors.bgRaised),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                if (count == 0) "·" else "$count",
                                style = Fm.text.labelMedium,
                                color = when {
                                    count == 0 -> Fm.colors.textTertiary
                                    diagonal -> Fm.colors.onAccent
                                    else -> Fm.colors.textPrimary
                                },
                            )
                        }
                    }
                }
            }
        }
    }
}
