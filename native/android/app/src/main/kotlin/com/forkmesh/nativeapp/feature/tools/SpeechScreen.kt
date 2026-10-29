package com.forkmesh.nativeapp.feature.tools

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.speech.tts.TextToSpeech
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
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
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.shell.FmIosSlider
import com.forkmesh.nativeapp.tools.SpeechEngineStatus
import com.forkmesh.nativeapp.tools.SpeechHighlight
import com.forkmesh.nativeapp.tools.SpeechPlaygroundModel
import com.forkmesh.nativeapp.tools.SpeechTuning
import com.forkmesh.nativeapp.tools.SpeechVoice
import com.forkmesh.nativeapp.tools.SpeechVoiceQuality
import com.forkmesh.nativeapp.tools.SpeechVoices
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatusLine
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Locale

class SpeechScreen : Screen() {
    override val title: String get() = SpeechPlaygroundModel.TITLE

    private var engine: AndroidSpeechEngine? = null
    private var model: SpeechPlaygroundModel? = null
    private var rev by mutableIntStateOf(0)

    private fun obtainModel(context: Context): SpeechPlaygroundModel {
        model?.let { return it }
        val created = AndroidSpeechEngine(context)
        engine = created
        return SpeechPlaygroundModel(created, Services.get<PrefStore>(), created.language).also {
            it.onChange = { rev++ }
            model = it
        }
    }

    override fun onDisposed() {
        model?.stop()
        engine?.shutdown()
        engine = null
        model = null
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val context = LocalContext.current
        val model = remember { obtainModel(context) }
        DisposableEffect(model) {
            model.load()
            rev++
            onDispose { model.stop() }
        }
        LaunchedEffect(model) {
            AppLifecycle.foreground.collect { foreground ->
                if (foreground) {
                    model.load()
                    rev++
                }
            }
        }
        Column(Modifier.fillMaxSize().background(Fm.colors.canvas)) {
            ToolsAppBar(title, navigator)
            when (model.status.takeIf { rev >= 0 }) {
                SpeechEngineStatus.FAILED -> FmEmptyState(
                    icon = "volume_up",
                    title = SpeechPlaygroundModel.UNAVAILABLE_TITLE,
                    message = SpeechPlaygroundModel.UNAVAILABLE_MESSAGE,
                )
                SpeechEngineStatus.STARTING, null -> FmSkeletonList()
                SpeechEngineStatus.READY -> SpeechContent(model, rev) { rev++ }
            }
        }
    }
}

private fun languageName(code: String): String {
    val locale = Locale.forLanguageTag(code)
    return locale.getDisplayName(Locale.getDefault()).ifEmpty { code }.replaceFirstChar { it.titlecase(Locale.getDefault()) }
}

private fun openVoiceSettings(context: Context) {
    val install = Intent(TextToSpeech.Engine.ACTION_INSTALL_TTS_DATA).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    try {
        context.startActivity(install)
    } catch (_: ActivityNotFoundException) {
        runCatching { context.startActivity(Intent("com.android.settings.TTS_SETTINGS").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
    }
}

private fun shareAudio(context: Context, file: File): Boolean = try {
    val uri = FileProvider.getUriForFile(context, PhotoPicker.authority(context), file)
    val send = Intent(Intent.ACTION_SEND)
        .setType("audio/wav")
        .putExtra(Intent.EXTRA_STREAM, uri)
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    send.clipData = ClipData.newRawUri(file.name, uri)
    context.startActivity(Intent.createChooser(send, file.name).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
    true
} catch (_: ActivityNotFoundException) {
    false
} catch (_: IllegalArgumentException) {
    false
}

@Composable
private fun SpeechContent(model: SpeechPlaygroundModel, rev: Int, changed: () -> Unit) {
    val context = LocalContext.current
    val focus = LocalFocusManager.current
    val scope = rememberCoroutineScope()
    var choosingVoice by remember { mutableStateOf(false) }
    var rendering by remember { mutableStateOf(false) }
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(FmSpace.x4),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x4),
    ) {
        SpeechTextCard(model, rev, changed)
        SpeechSamples(model) {
            focus.clearFocus()
            changed()
        }
        SpeechControls(
            model = model,
            rev = rev,
            rendering = rendering,
            onPlay = {
                focus.clearFocus()
                if (model.previewing != null) model.stop() else model.toggle()
                changed()
            },
            onStop = {
                model.stop()
                changed()
            },
            onSave = {
                focus.clearFocus()
                rendering = true
                scope.launch {
                    val file = withContext(Dispatchers.IO) { model.renderAudio() }
                    rendering = false
                    if (file != null && !shareAudio(context, file)) {
                        model.notice = com.forkmesh.nativeapp.tools.SpeechNotice("No app on this phone can take the audio file.", isError = true)
                    }
                    changed()
                }
            },
        )
        val notice = model.notice
        if (notice != null) {
            FmInlineStatusLine(notice.message, notice.isError, onDismiss = {
                model.notice = null
                changed()
            })
        }
        SpeechVoiceCard(model, rev) { choosingVoice = true }
        SpeechTuningCard(model, rev, changed)
        if (model.showsVoiceHint) {
            SpeechHint(SpeechPlaygroundModel.NO_NATURAL_VOICE, "download_outlined") { openVoiceSettings(context) }
        }
    }
    if (choosingVoice) {
        SpeechVoiceSheet(model, rev, changed) {
            if (model.previewing != null) model.stop()
            choosingVoice = false
            changed()
        }
    }
}

@Composable
private fun SpeechTextCard(model: SpeechPlaygroundModel, rev: Int, changed: () -> Unit) {
    val c = Fm.colors
    val interaction = remember { MutableInteractionSource() }
    val editing by interaction.collectIsFocusedAsState()
    val highlight = model.highlight.takeIf { rev >= 0 }
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.lg))
            .background(c.bgRaised)
            .border(if (editing) 1.2.dp else 1.dp, if (editing) c.accent else c.border, RoundedCornerShape(FmRadius.lg)),
    ) {
        if (highlight != null) {
            Text(
                highlighted(highlight, c.accent, c.accentSubtle),
                style = Fm.text.bodyLarge.copy(color = c.textPrimary),
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 168.dp)
                    .clickable {
                        model.stop()
                        changed()
                    }
                    .padding(FmSpace.x3)
                    .testTag("speech-highlight"),
            )
        } else {
            Box(Modifier.fillMaxWidth().heightIn(min = 168.dp, max = 320.dp).padding(FmSpace.x3)) {
                if (model.text.isEmpty()) {
                    Text(SpeechPlaygroundModel.PLACEHOLDER, style = Fm.text.bodyLarge.copy(color = c.textTertiary))
                }
                BasicTextField(
                    value = model.text,
                    onValueChange = {
                        model.edit(it)
                        changed()
                    },
                    textStyle = Fm.text.bodyLarge.copy(color = c.textPrimary),
                    cursorBrush = SolidColor(c.accent),
                    interactionSource = interaction,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 144.dp)
                        .testTag("speech-text")
                        .semantics { contentDescription = "Text to speak" },
                )
            }
        }
        Row(
            Modifier.fillMaxWidth().padding(start = FmSpace.x3, end = FmSpace.x1),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                model.summary,
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
                modifier = Modifier.weight(1f).padding(vertical = FmSpace.x3),
            )
            if (model.text.isNotEmpty()) {
                FmTextButton(
                    onClick = {
                        model.clear()
                        changed()
                    },
                    modifier = Modifier.testTag("speech-clear"),
                ) { Text("Clear") }
            }
        }
    }
}

private fun highlighted(highlight: SpeechHighlight, accent: Color, subtle: Color) = buildAnnotatedString {
    append(highlight.before)
    withStyle(SpanStyle(color = accent, background = subtle, fontWeight = FontWeight.SemiBold)) { append(highlight.word) }
    append(highlight.after)
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun SpeechSamples(model: SpeechPlaygroundModel, changed: () -> Unit) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        SpeechPlaygroundModel.SAMPLES.forEach { sample ->
            FmToneButton(
                sample.title,
                onPressed = {
                    model.useSample(sample)
                    changed()
                },
                modifier = Modifier.testTag("speech-sample-${sample.title.lowercase()}"),
            )
        }
    }
}

@Composable
private fun SpeechControls(
    model: SpeechPlaygroundModel,
    rev: Int,
    rendering: Boolean,
    onPlay: () -> Unit,
    onStop: () -> Unit,
    onSave: () -> Unit,
) {
    val c = Fm.colors
    val label = when (model.state.takeIf { rev >= 0 }) {
        SpeechPlaygroundModel.State.IDLE -> "Speak"
        SpeechPlaygroundModel.State.SPEAKING -> if (model.previewing == null) "Pause" else "Stop preview"
        SpeechPlaygroundModel.State.PAUSED, null -> "Resume"
    }
    val icon = if (model.state == SpeechPlaygroundModel.State.SPEAKING) "pause_rounded" else "play_arrow_rounded"
    val enabled = model.canSpeak || model.state != SpeechPlaygroundModel.State.IDLE
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
        Button(
            onClick = onPlay,
            enabled = enabled,
            modifier = Modifier.weight(1f).heightIn(min = 52.dp).testTag("speech-play"),
            shape = RoundedCornerShape(50),
            colors = ButtonDefaults.buttonColors(
                containerColor = c.accent,
                contentColor = Color.White,
                disabledContainerColor = c.accent.copy(alpha = .3f),
                disabledContentColor = Color.White.copy(alpha = .7f),
            ),
            elevation = null,
        ) {
            FmIcon(icon, size = 22.dp)
            Spacer(Modifier.width(FmSpace.x2))
            Text(label, style = Fm.text.titleSmall.copy(color = Color.White))
        }
        if (model.state != SpeechPlaygroundModel.State.IDLE) {
            FmIconButton("stop_rounded", "Stop", onPressed = onStop, color = c.textPrimary, modifier = Modifier.testTag("speech-stop"))
        }
        if (rendering) {
            Box(Modifier.size(48.dp), contentAlignment = Alignment.Center) { FmCircularProgressIndicator(Modifier.size(22.dp)) }
        } else {
            FmIconButton(
                "ios_share",
                "Save audio",
                onPressed = if (model.canSpeak) onSave else null,
                color = c.textPrimary,
                modifier = Modifier.testTag("speech-save"),
            )
        }
    }
}

@Composable
private fun SpeechQualityBadge(voice: SpeechVoice) {
    val c = Fm.colors
    when (voice.quality) {
        SpeechVoiceQuality.PREMIUM -> FmStatusBadge("Premium", c.success)
        SpeechVoiceQuality.ENHANCED -> FmStatusBadge("Enhanced", c.warning)
        SpeechVoiceQuality.STANDARD -> Unit
    }
    if (voice.online) {
        Spacer(Modifier.width(FmSpace.x1))
        FmStatusBadge("Online", c.accent)
    }
}

@Composable
private fun SpeechVoiceCard(model: SpeechPlaygroundModel, rev: Int, onOpen: () -> Unit) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth().testTag("speech-voice"), onTap = onOpen, radius = FmRadius.lg, padding = PaddingValues(FmSpace.x4)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(36.dp).clip(CircleShape).background(c.accentSubtle),
                contentAlignment = Alignment.Center,
            ) { FmIcon("graphic_eq", size = 20.dp, tint = c.accent) }
            Spacer(Modifier.width(FmSpace.x3))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text("Voice", style = Fm.text.labelSmall.copy(color = c.textSecondary))
                val voice = model.voice.takeIf { rev >= 0 }
                if (voice != null) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            voice.name,
                            style = Fm.text.titleSmall.copy(color = c.textPrimary),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f, fill = false),
                        )
                        Spacer(Modifier.width(FmSpace.x2))
                        SpeechQualityBadge(voice)
                    }
                    Text(languageName(voice.language), style = Fm.text.bodySmall.copy(color = c.textSecondary))
                } else {
                    Text(SpeechPlaygroundModel.NO_VOICES, style = Fm.text.bodyMedium.copy(color = c.textSecondary))
                }
            }
            FmIcon("chevron_right", size = 20.dp, tint = c.textTertiary)
        }
    }
}

@Composable
private fun SpeechTuningCard(model: SpeechPlaygroundModel, rev: Int, changed: () -> Unit) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(FmSpace.x4)) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Delivery", style = Fm.text.labelSmall.copy(color = c.textSecondary), modifier = Modifier.weight(1f))
                if (model.tuned && rev >= 0) {
                    FmTextButton(onClick = {
                        model.resetTuning()
                        changed()
                    }) { Text("Reset") }
                }
            }
            SpeechSlider(
                "Speed",
                model.speed,
                SpeechTuning.MIN_SPEED,
                SpeechTuning.MAX_SPEED,
                SpeechTuning.speedLabel(model.speed),
                "speech-speed",
                onChange = {
                    model.speed = it
                    changed()
                },
                onDone = { model.tuningChanged() },
            )
            SpeechSlider(
                "Pitch",
                model.pitch,
                SpeechTuning.MIN_PITCH,
                SpeechTuning.MAX_PITCH,
                SpeechTuning.pitchLabel(model.pitch),
                "speech-pitch",
                onChange = {
                    model.pitch = it
                    changed()
                },
                onDone = { model.tuningChanged() },
            )
        }
    }
}

@Composable
private fun SpeechSlider(
    title: String,
    value: Double,
    min: Double,
    max: Double,
    caption: String,
    tag: String,
    onChange: (Double) -> Unit,
    onDone: () -> Unit,
) {
    val c = Fm.colors
    Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(title, style = Fm.text.bodyMedium.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
            Text(caption, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        }
        FmIosSlider(
            value = ((value - min) / (max - min)).toFloat(),
            onValueChange = { onChange(SpeechTuning.snap(min + it * (max - min)).coerceIn(min, max)) },
            onValueChangeFinished = onDone,
            modifier = Modifier
                .fillMaxWidth()
                .testTag(tag)
                .semantics {
                    contentDescription = title
                    stateDescription = caption
                },
        )
    }
}

@Composable
private fun SpeechHint(text: String, icon: String, onAction: () -> Unit) {
    val c = Fm.colors
    FmCard(Modifier.fillMaxWidth(), radius = FmRadius.lg, padding = PaddingValues(FmSpace.x4)) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
            Row(verticalAlignment = Alignment.Top) {
                FmIcon(icon, size = 20.dp, tint = c.accent)
                Spacer(Modifier.width(FmSpace.x3))
                Text(text, style = Fm.text.bodyMedium.copy(color = c.textSecondary), modifier = Modifier.weight(1f))
            }
            FmToneButton(SpeechPlaygroundModel.GET_VOICES, onPressed = onAction, modifier = Modifier.testTag("speech-get-voices"))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SpeechVoiceSheet(model: SpeechPlaygroundModel, rev: Int, changed: () -> Unit, onDismiss: () -> Unit) {
    val c = Fm.colors
    val sections = remember(model.voices, model.language, rev) { SpeechVoices.sections(model.voices, model.language, ::languageName) }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Row(
            Modifier.fillMaxWidth().padding(start = FmSpace.x4, end = FmSpace.x2, top = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Voices", style = Fm.text.titleMedium.copy(color = c.textPrimary), modifier = Modifier.weight(1f))
            FmTextButton(onClick = onDismiss) { Text("Done") }
        }
        LazyColumn(Modifier.fillMaxWidth(), contentPadding = PaddingValues(bottom = FmSpace.x6)) {
            if (model.showsVoiceHint) {
                item(key = "hint") {
                    Text(
                        SpeechPlaygroundModel.NO_NATURAL_VOICE,
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                    )
                }
            }
            sections.forEach { section ->
                item(key = "section-${section.id}") {
                    Text(
                        section.title.uppercase(Locale.getDefault()),
                        style = Fm.text.labelSmall.copy(color = c.textTertiary),
                        modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x1),
                    )
                }
                items(section.voices, key = { "voice-${it.id}" }) { voice ->
                    SpeechVoiceRow(model, voice, rev, changed)
                }
            }
        }
    }
}

@Composable
private fun SpeechVoiceRow(model: SpeechPlaygroundModel, voice: SpeechVoice, rev: Int, changed: () -> Unit) {
    val c = Fm.colors
    val selected = model.voiceId == voice.id && rev >= 0
    val playing = model.previewing == voice.id && model.state != SpeechPlaygroundModel.State.IDLE && rev >= 0
    Row(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            Modifier
                .weight(1f)
                .clip(RoundedCornerShape(FmRadius.md))
                .clickable {
                    model.select(voice)
                    changed()
                }
                .semantics { this.selected = selected }
                .padding(horizontal = FmSpace.x2, vertical = FmSpace.x3),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon("check", size = 18.dp, tint = if (selected) c.accent else Color.Transparent)
            Spacer(Modifier.width(FmSpace.x2))
            Text(
                voice.name,
                style = Fm.text.bodyLarge.copy(color = c.textPrimary),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f, fill = false),
            )
            Spacer(Modifier.width(FmSpace.x2))
            SpeechQualityBadge(voice)
        }
        FmIconButton(
            if (playing) "stop_circle" else "play_circle_outline",
            if (playing) "Stop preview" else "Preview ${voice.name}",
            onPressed = {
                model.preview(voice)
                changed()
            },
            color = c.accent,
        )
    }
}
