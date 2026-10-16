@file:OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)

package com.forkmesh.nativeapp.feature.composer

import com.forkmesh.nativeapp.ui.kit.fmGlass
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.content.MediaType
import androidx.compose.foundation.content.contentReceiver
import androidx.compose.foundation.content.hasMediaType
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.isCtrlPressed
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalTextToolbar
import com.forkmesh.nativeapp.composer.PromptPaste
import com.forkmesh.nativeapp.composer.PromptPasteRules
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.kit.FmUserActivity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import android.net.Uri
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentModelChoice
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.PromptTextFile
import com.forkmesh.nativeapp.agents.PromptTextFiles
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import com.forkmesh.nativeapp.composer.ComposerRules
import com.forkmesh.nativeapp.composer.DictationText
import com.forkmesh.nativeapp.composer.Markup
import com.forkmesh.nativeapp.composer.PhotoPrep
import com.forkmesh.nativeapp.composer.SlashCommand
import com.forkmesh.nativeapp.composer.SlashCommands
import com.forkmesh.nativeapp.feature.agents.AgentPhotoPicker
import com.forkmesh.nativeapp.feature.tools.vault.findActivity
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class ComposerCommand(val command: SlashCommand, val run: () -> Unit)

private enum class AttachChoice { Library, Camera, File, Screen, TextFile }

private val ControlSize = 44.dp
private const val CAPTURE_SETTLE_MILLIS = 250L
private val ComposerRadius = 28.dp

private class ComposerPalette(
    val surface: Color,
    val border: Color,
    val control: Color,
    val controlActive: Color,
    val text: Color,
    val muted: Color,
    val primary: Color,
    val onPrimary: Color,
)

@Composable
private fun palette(): ComposerPalette {
    val c = Fm.colors
    return ComposerPalette(
        surface = if (c.dark) c.bgOverlay.copy(alpha = 0.92f) else c.bgRaised.copy(alpha = 0.96f),
        border = if (c.dark) Color.White.copy(alpha = 0.10f) else Color.Black.copy(alpha = 0.08f),
        control = if (c.dark) Color.White.copy(alpha = 0.08f) else Color.Black.copy(alpha = 0.05f),
        controlActive = c.accentSubtle,
        text = c.textPrimary,
        muted = c.textTertiary,
        primary = c.textPrimary,
        onPrimary = c.bgBase,
    )
}

@Composable
fun PromptComposer(
    text: String,
    onTextChange: (String) -> Unit,
    onSend: (() -> Unit)?,
    modifier: Modifier = Modifier,
    hint: String = "Type / for commands",
    running: Boolean = false,
    busy: Boolean = false,
    onStop: (() -> Unit)? = null,
    modelLabel: String? = null,
    onPickModel: (() -> Unit)? = null,
    modelChoices: List<AgentModelChoice> = emptyList(),
    selectedModel: AgentModelChoice? = null,
    onSelectModel: ((AgentModelChoice) -> Unit)? = null,
    attachments: List<PromptAttachment> = emptyList(),
    onAttachmentsChanged: ((List<PromptAttachment>) -> Unit)? = null,
    onPasteImages: ((PromptPaste) -> Unit)? = null,
    allowFileAttachments: Boolean = true,
    uploadsFiles: Boolean = false,
    dictationEnabled: Boolean = true,
    commands: List<ComposerCommand> = emptyList(),
    extraControls: @Composable RowScope.() -> Unit = {},
    header: (@Composable () -> Unit)? = null,
    autofocus: Boolean = false,
    maxLength: Int = Int.MAX_VALUE,
    maxInputHeight: Dp = 168.dp,
    submitOnEnter: Boolean = false,
    sendDescription: String = "Send",
    padForKeyboard: Boolean = true,
    outerPadding: Boolean = true,
    onSendBounds: ((Rect) -> Unit)? = null,
    offersFix: Boolean = false,
) {
    val colors = palette()
    val pop = remember { Animatable(1f) }
    val messenger = LocalFmMessenger.current
    val status = rememberFmStatus()
    val scope = rememberCoroutineScope()
    val picker = rememberPhotoPicker()
    val dictation = rememberSpeechDictation()
    var picking by remember { mutableStateOf(false) }
    var modelMenu by remember { mutableStateOf(false) }
    val offersModelMenu = modelChoices.isNotEmpty() && onSelectModel != null
    val openModel: (() -> Unit)? = if (offersModelMenu) ({ modelMenu = true }) else onPickModel
    var dictationBase by remember { mutableStateOf("") }
    val quietSend = remember { mutableStateOf<() -> Unit>({}) }
    var field by remember { mutableStateOf(TextFieldValue(text, TextRange(text.length))) }
    if (field.text != text) field = TextFieldValue(text, TextRange(text.length))
    val focus = remember { FocusRequester() }
    val canAttach = onAttachmentsChanged != null
    val canPaste = canAttach || onPasteImages != null
    val context = LocalContext.current
    val baseToolbar = LocalTextToolbar.current
    val latestAttachments by rememberUpdatedStateCompat(attachments)
    val latestChanged by rememberUpdatedStateCompat(onAttachmentsChanged)
    val latestPaste by rememberUpdatedStateCompat(onPasteImages)
    var textFiles by remember { mutableStateOf<List<PromptTextFile>>(emptyList()) }
    val latestText by rememberUpdatedStateCompat(text)

    fun showError(message: String) = status.error(message)

    fun addTextFiles(uris: List<Uri>) {
        if (uris.isEmpty()) return
        scope.launch {
            picking = true
            try {
                for (uri in uris) {
                    val file = try {
                        TextFileIntake.read(context, uri)
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (error: Exception) {
                        showError(TextFileIntake.message(error))
                        continue
                    }
                    val refusal = PromptTextFiles.problem(file, textFiles, latestText)
                    if (refusal != null) showError(refusal) else textFiles = textFiles + file
                }
            } finally {
                picking = false
            }
        }
    }

    val textPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris -> addTextFiles(uris) }

    fun openTextFile() {
        if (!uploadsFiles || picking) return
        if (textFiles.size >= PromptTextFiles.MAX_COUNT) {
            showError(PromptTextFiles.fullMessage())
            return
        }
        runCatching { textPicker.launch(arrayOf("*/*")) }.onFailure { showError("Could not open your files: ${it.message ?: it}") }
    }

    fun pick(source: PhotoSource) {
        val changed = latestChanged ?: return
        if (picking) return
        val room = PhotoPrep.roomFor(latestAttachments.size)
        if (room <= 0) {
            showError(PhotoPrep.fullMessage())
            return
        }
        scope.launch {
            picking = true
            try {
                val prepared = picker.pick(source, room, ::showError)
                if (prepared.isNotEmpty()) changed((latestAttachments + prepared).take(com.forkmesh.nativeapp.agents.PromptAttachmentLimits.MAX_COUNT))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                showError(
                    if (source == PhotoSource.Camera) "The camera is not available: ${error.message ?: error}"
                    else "Could not open your photos: ${error.message ?: error}",
                )
            } finally {
                picking = false
            }
        }
    }

    fun markUp(source: Bitmap, name: String, replacing: PromptAttachment?) {
        latestChanged ?: return
        scope.launch {
            val marked = messenger.markUp(source, name, if (replacing == null) "Add to prompt" else "Done") ?: return@launch
            val changed = latestChanged ?: return@launch
            val current = latestAttachments
            if (replacing != null && current.any { it.id == replacing.id }) {
                changed(current.map { if (it.id == replacing.id) marked else it })
            } else if (PhotoPrep.roomFor(current.size) > 0) {
                changed(current + marked)
            } else {
                showError(PhotoPrep.fullMessage())
            }
        }
    }

    fun captureScreen() {
        latestChanged ?: return
        if (picking) return
        if (PhotoPrep.roomFor(latestAttachments.size) <= 0) {
            showError(PhotoPrep.fullMessage())
            return
        }
        val activity = context.findActivity() ?: return showError("This screen could not be captured.")
        scope.launch {
            delay(CAPTURE_SETTLE_MILLIS)
            val shot = try {
                ScreenCapture.window(activity)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                showError(error.message ?: "This screen could not be captured.")
                return@launch
            }
            markUp(shot, Markup.CAPTURE_NAME, null)
        }
    }

    fun editAttachment(attachment: PromptAttachment) {
        if (picking) return
        scope.launch {
            val source = withContext(Dispatchers.Default) { MarkupRender.decode(attachment) }
                ?: return@launch showError("That photo could not be opened for markup.")
            markUp(source, attachment.name, attachment)
        }
    }

    fun insertText(inserted: String) {
        if (inserted.isEmpty()) return
        val start = minOf(field.selection.start, field.selection.end).coerceIn(0, field.text.length)
        val end = maxOf(field.selection.start, field.selection.end).coerceIn(0, field.text.length)
        val next = field.text.substring(0, start) + inserted + field.text.substring(end)
        if (next.length > maxLength) return
        field = TextFieldValue(next, TextRange(start + inserted.length))
        onTextChange(next)
    }

    fun pasteImages(paste: PromptPaste) {
        val sendPasted = latestPaste
        if (latestChanged == null && sendPasted != null) {
            insertText(paste.text)
            sendPasted(paste)
            return
        }
        val changed = latestChanged ?: return
        if (picking) return
        val admitted = PromptPasteRules.admit(paste.images, latestAttachments.size)
        admitted.problem?.let(::showError)
        insertText(paste.text)
        if (admitted.accepted.isEmpty()) {
            if (admitted.problem == null) showError(PhotoPrep.fullMessage())
            return
        }
        scope.launch {
            picking = true
            try {
                val prepared = ArrayList<PromptAttachment>()
                for (image in admitted.accepted) {
                    try {
                        prepared.add(withContext(Dispatchers.Default) { PhotoIntake.prepare(image.data, image.name) })
                    } catch (error: Exception) {
                        if (error is CancellationException) throw error
                        showError(error.message ?: "That image could not be attached.")
                    }
                }
                if (prepared.isNotEmpty()) {
                    changed((latestAttachments + prepared).take(com.forkmesh.nativeapp.agents.PromptAttachmentLimits.MAX_COUNT))
                }
            } finally {
                picking = false
            }
        }
    }

    fun pasteFromClipboard(fallback: (() -> Unit)?) {
        scope.launch {
            val paste = PromptPasteboard.read(context)
            if (paste.images.isEmpty()) fallback?.invoke() else pasteImages(paste)
        }
    }

    fun openAttachSheet() {
        if (picking) return
        if (!uploadsFiles && PhotoPrep.roomFor(latestAttachments.size) <= 0) {
            showError(PhotoPrep.fullMessage())
            return
        }
        scope.launch {
            val choice = messenger.showModalBottomSheet<AttachChoice> { close ->
                Column(Modifier.padding(bottom = FmSpace.x5)) {
                    Text(
                        if (uploadsFiles) "Add to prompt" else "Add photos",
                        Modifier.padding(FmSpace.x4),
                        style = Fm.text.titleMedium.copy(color = Fm.colors.textPrimary),
                    )
                    if (canAttach) {
                        SourceRow("photo_library_outlined", "Photo library") { close(AttachChoice.Library) }
                        SourceRow("photo_camera_outlined", "Camera") { close(AttachChoice.Camera) }
                        SourceRow("screenshot", "Capture screen") { close(AttachChoice.Screen) }
                        if (allowFileAttachments) SourceRow("folder_outlined", "File") { close(AttachChoice.File) }
                    }
                    if (uploadsFiles) SourceRow("description_outlined", "Text file") { close(AttachChoice.TextFile) }
                }
            }
            when (choice) {
                AttachChoice.Library -> pick(PhotoSource.Library)
                AttachChoice.Camera -> pick(PhotoSource.Camera)
                AttachChoice.File -> pick(PhotoSource.File)
                AttachChoice.Screen -> captureScreen()
                AttachChoice.TextFile -> openTextFile()
                null -> {}
            }
        }
    }

    fun toggleDictation() {
        if (dictation.listening) {
            dictation.stop()
            return
        }
        dictationBase = field.text
        dictation.start(
            onWords = { words ->
                val merged = ComposerRules.clamp(DictationText.merge(dictationBase, words), maxLength)
                field = TextFieldValue(merged, TextRange(merged.length))
                onTextChange(merged)
            },
            onError = ::showError,
            onQuiet = { quietSend.value() },
        )
    }

    DisposableEffect(canAttach) {
        if (canAttach) {
            val previous = AgentPhotoPicker.pick
            AgentPhotoPicker.pick = { picker.pick(PhotoSource.Library, PhotoPrep.roomFor(latestAttachments.size), ::showError) }
            onDispose { if (previous == null) AgentPhotoPicker.pick = null else AgentPhotoPicker.pick = previous }
        } else {
            onDispose {}
        }
    }

    LaunchedEffect(autofocus) { if (autofocus) runCatching { focus.requestFocus() } }

    val allCommands = buildList {
        val builtIn = SlashCommands.builtIn(openModel != null, canAttach, dictationEnabled, uploadsFiles)
        for (command in builtIn) {
            add(
                ComposerCommand(command) {
                    when (command) {
                        SlashCommands.MODEL -> openModel?.invoke()
                        SlashCommands.PHOTO -> pick(PhotoSource.Library)
                        SlashCommands.CAMERA -> pick(PhotoSource.Camera)
                        SlashCommands.SCREENSHOT -> captureScreen()
                        SlashCommands.DICTATE -> toggleDictation()
                        SlashCommands.FILE -> openTextFile()
                    }
                },
            )
        }
        addAll(commands)
    }
    val matches = SlashCommands.matching(field.text, allCommands.map { it.command })
    val carried = attachments.size + textFiles.size
    val hasContent = ComposerRules.hasContent(field.text, carried)
    val showStop = ComposerRules.showsStop(running, field.text, carried, onStop != null)
    val canSend = ComposerRules.canSend(field.text, carried, busy, onSend != null)

    fun submit() {
        if (!ComposerRules.hasContent(field.text, attachments.size + textFiles.size) || busy) return
        if (dictation.listening) dictation.stop()
        if (onSend != null && textFiles.isNotEmpty()) {
            val over = PromptTextFiles.overBudget(field.text, textFiles)
            if (over != null) {
                showError(over)
                return
            }
            val folded = PromptTextFiles.fold(field.text, textFiles)
            textFiles = emptyList()
            field = TextFieldValue(folded, TextRange(folded.length))
            onTextChange(folded)
        }
        if (onSend != null) FmSoundCues.play(SoundCue.SEND)
        if (onSend != null && onSendBounds != null) {
            scope.launch {
                pop.animateTo(0.8f, tween(90))
                pop.animateTo(1.14f, tween(160))
                pop.animateTo(1f, spring(dampingRatio = Spring.DampingRatioMediumBouncy))
            }
        }
        onSend?.invoke()
    }
    quietSend.value = ::submit

    fun submitFix() {
        if (!ComposerRules.hasContent(field.text, attachments.size + textFiles.size) || busy || onSend == null) return
        if (dictation.listening) dictation.stop()
        val asked = ComposerRules.clamp(ComposerRules.fixPrompt(field.text), maxLength)
        field = TextFieldValue(asked, TextRange(asked.length))
        onTextChange(asked)
        submit()
    }

    Column(
        modifier
            .fillMaxWidth()
            .then(if (padForKeyboard) Modifier.navigationBarsPadding().imePadding() else Modifier)
            .then(if (outerPadding) Modifier.padding(start = FmSpace.x3, end = FmSpace.x3, top = FmSpace.x1, bottom = FmSpace.x2) else Modifier),
    ) {
        if (matches.isNotEmpty()) {
            Column(
                Modifier
                    .padding(bottom = FmSpace.x2)
                    .fillMaxWidth()
                    .fmGlass(RoundedCornerShape(FmRadius.lg + 4.dp), tint = colors.surface)
                    .padding(vertical = FmSpace.x1),
            ) {
                for (name in matches) {
                    val entry = allCommands.first { it.command == name }
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(min = ControlSize)
                            .clickable {
                                field = TextFieldValue("")
                                onTextChange("")
                                entry.run()
                            }
                            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text("/${name.name}", style = Fm.text.bodySmall.copy(color = colors.text, fontFamily = FontFamily.Monospace))
                        Spacer(Modifier.width(FmSpace.x3))
                        Text(
                            name.description,
                            Modifier.weight(1f),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            style = Fm.text.bodySmall.copy(color = colors.muted),
                        )
                    }
                }
            }
        }
        Column(
            Modifier
                .fillMaxWidth()
                .fmGlass(RoundedCornerShape(ComposerRadius), tint = colors.surface)
                .padding(FmSpace.x3),
        ) {
            header?.invoke()
            FmInlineStatus(status, Modifier.padding(bottom = FmSpace.x2))
            if (attachments.isNotEmpty() || picking) {
                Thumbnails(
                    attachments,
                    picking,
                    colors,
                    onMarkUp = ::editAttachment,
                    onRemove = { removed -> onAttachmentsChanged?.invoke(attachments.filter { it.id != removed.id }) },
                )
            }
            if (textFiles.isNotEmpty()) {
                TextFileChips(textFiles, colors, onRemove = { removed -> textFiles = textFiles.filter { it.id != removed.id } })
            }
            val textStyle = Fm.text.bodyLarge.copy(color = colors.text)
            Box(Modifier.fillMaxWidth().heightIn(max = maxInputHeight).padding(horizontal = FmSpace.x1, vertical = FmSpace.x1)) {
                if (field.text.isEmpty()) {
                    Text(if (dictation.listening) "Listening…" else hint, style = textStyle.copy(color = colors.muted), maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                val pasteToolbar = remember(baseToolbar, canPaste) {
                    if (!canPaste) {
                        baseToolbar
                    } else {
                        ImagePasteTextToolbar(
                            base = baseToolbar,
                            hasImages = { PromptPasteboard.hasImages(context) },
                            pasteImages = { fallback -> pasteFromClipboard(fallback) },
                        )
                    }
                }
                val receiver = if (!canPaste) {
                    Modifier
                } else {
                    Modifier
                        .contentReceiver { content ->
                            val clip = content.clipEntry.clipData
                            val embedded = (0 until clip.itemCount).any {
                                clip.getItemAt(it).htmlText?.contains("data:image/", ignoreCase = true) == true
                            }
                            if (!content.hasMediaType(MediaType.Image) && !embedded) {
                                return@contentReceiver content
                            }
                            scope.launch {
                                val paste = PromptPasteboard.read(context, clip)
                                if (paste.images.isEmpty()) insertText(paste.text) else pasteImages(paste)
                            }
                            null
                        }
                        .onPreviewKeyEvent { event ->
                            val paste = event.type == KeyEventType.KeyDown && event.isCtrlPressed && event.key == Key.V
                            if (paste && PromptPasteboard.hasImages(context)) {
                                pasteFromClipboard(null)
                                true
                            } else {
                                false
                            }
                        }
                }
                CompositionLocalProvider(LocalTextToolbar provides pasteToolbar) {
                BasicTextField(
                    value = field,
                    onValueChange = FmUserActivity.typing { next ->
                        if (next.text.length <= maxLength) {
                            if (next.text.length != field.text.length && !dictation.listening) FmSoundCues.play(SoundCue.KEYSTROKE)
                            field = next
                            if (next.text != text) onTextChange(next.text)
                        }
                    },
                    modifier = Modifier.fillMaxWidth().then(receiver).focusRequester(focus).semantics { contentDescription = hint },
                    textStyle = textStyle,
                    cursorBrush = SolidColor(colors.text),
                    minLines = 1,
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.Sentences,
                        imeAction = if (submitOnEnter) ImeAction.Send else ImeAction.Default,
                    ),
                    keyboardActions = KeyboardActions(onSend = { submit() }),
                    singleLine = false,
                )
                }
            }
            Spacer(Modifier.height(FmSpace.x2))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                    if (canAttach || uploadsFiles) {
                        RoundButton("add", if (canAttach) "Add photos" else "Add a file", colors, onClick = if (picking) null else ({ openAttachSheet() }))
                        Spacer(Modifier.width(FmSpace.x2))
                    }
                    extraControls()
                    if (modelLabel != null) {
                        Box(Modifier.weight(1f, fill = false)) {
                            ModelPill(modelLabel, colors, openModel)
                            if (offersModelMenu) {
                                ModelMenu(modelMenu, modelChoices, selectedModel, onDismiss = { modelMenu = false }) { picked ->
                                    modelMenu = false
                                    onSelectModel(picked)
                                }
                            }
                        }
                    }
                }
                Spacer(Modifier.width(FmSpace.x2))
                if (offersFix && onSend != null) {
                    FixPill(colors, enabled = canSend, onClick = { submitFix() })
                    Spacer(Modifier.width(FmSpace.x2))
                }
                if (showStop) {
                    RoundButton("stop_rounded", "Stop", colors, primary = true, onClick = onStop)
                } else {
                    RoundButton(
                        if (busy) null else "arrow_upward_rounded",
                        sendDescription,
                        colors,
                        primary = true,
                        dimmed = !canSend,
                        onClick = if (onSend == null) null else ({ submit() }),
                        modifier = Modifier
                            .graphicsLayer {
                                scaleX = pop.value
                                scaleY = pop.value
                            }
                            .then(if (onSendBounds != null) Modifier.onGloballyPositioned { onSendBounds(it.boundsInRoot()) } else Modifier),
                    )
                }
            }
        }
    }
}

@Composable
private fun <T> rememberUpdatedStateCompat(value: T) = androidx.compose.runtime.rememberUpdatedState(value)

@Composable
private fun SourceRow(icon: String, label: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 52.dp)
            .clickable(onClick = onClick)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 22.dp, tint = Fm.colors.textSecondary)
        Spacer(Modifier.width(FmSpace.x3))
        Text(label, style = Fm.text.bodyLarge.copy(color = Fm.colors.textPrimary))
    }
}

@Composable
private fun RoundButton(
    icon: String?,
    description: String,
    colors: ComposerPalette,
    primary: Boolean = false,
    active: Boolean = false,
    dimmed: Boolean = false,
    onClick: (() -> Unit)?,
    modifier: Modifier = Modifier,
) {
    val enabled = onClick != null && !dimmed
    val fill = when {
        primary && enabled -> colors.primary
        active -> colors.controlActive
        else -> colors.control
    }
    val fg = when {
        primary && enabled -> colors.onPrimary
        enabled -> colors.text
        else -> colors.muted
    }
    Box(
        modifier
            .size(ControlSize)
            .clip(CircleShape)
            .background(fill)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .semantics {
                role = Role.Button
                contentDescription = description
            },
        contentAlignment = Alignment.Center,
    ) {
        if (icon == null) {
            FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        } else {
            FmIcon(icon, size = 22.dp, tint = fg)
        }
    }
}

@Composable
fun ComposerIconButton(icon: String, description: String, active: Boolean = false, onClick: () -> Unit) {
    RoundButton(icon, description, palette(), active = active, onClick = onClick)
}

@Composable
private fun FixPill(colors: ComposerPalette, enabled: Boolean, onClick: () -> Unit) {
    val fg = if (enabled) colors.text else colors.muted
    Row(
        Modifier
            .heightIn(min = ControlSize)
            .clip(RoundedCornerShape(FmRadius.full))
            .background(colors.control)
            .clickable(onClick = onClick)
            .semantics {
                role = Role.Button
                contentDescription = "Send as Please fix"
            }
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("build_rounded", size = 18.dp, tint = fg)
        Spacer(Modifier.width(FmSpace.x1))
        Text("Fix", maxLines = 1, style = Fm.text.bodyMedium.copy(color = fg))
    }
}

@Composable
private fun ModelPill(label: String, colors: ComposerPalette, onClick: (() -> Unit)?, modifier: Modifier = Modifier) {
    Row(
        modifier
            .heightIn(min = ControlSize)
            .clip(RoundedCornerShape(FmRadius.full))
            .background(colors.control)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .semantics { contentDescription = "Model: $label" }
            .padding(horizontal = FmSpace.x4),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            label,
            Modifier.weight(1f, fill = false),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = Fm.text.bodyMedium.copy(color = colors.text),
        )
        if (onClick != null) {
            Spacer(Modifier.width(FmSpace.x1))
            FmIcon("keyboard_arrow_down_rounded", size = 18.dp, tint = colors.muted)
        }
    }
}

@Composable
private fun ModelMenu(
    expanded: Boolean,
    choices: List<AgentModelChoice>,
    selected: AgentModelChoice?,
    onDismiss: () -> Unit,
    onPick: (AgentModelChoice) -> Unit,
) {
    val c = Fm.colors
    FmDropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        for (choice in choices) {
            val chosen = choice.alias == selected?.alias
            val detail = listOf(if (choice.provider == "codex") "Codex" else "Claude Code", choice.note)
                .filter { it.isNotEmpty() }
                .joinToString(" · ")
            DropdownMenuItem(
                text = {
                    Column {
                        Text(choice.label, style = Fm.text.bodyMedium.copy(color = c.textPrimary))
                        Text(detail, style = Fm.text.bodySmall.copy(color = c.textSecondary))
                    }
                },
                onClick = {
                    FmSoundCues.play(SoundCue.CLICK)
                    onPick(choice)
                },
                modifier = Modifier.semantics { this.selected = chosen },
                trailingIcon = if (chosen) ({ FmIcon("check", size = 18.dp, tint = c.accent) }) else null,
            )
        }
    }
}

@Composable
private fun TextFileChips(files: List<PromptTextFile>, colors: ComposerPalette, onRemove: (PromptTextFile) -> Unit) {
    Row(
        Modifier
            .padding(bottom = FmSpace.x2)
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (file in files) {
            Row(
                Modifier
                    .clip(RoundedCornerShape(FmRadius.full))
                    .background(colors.control)
                    .padding(start = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                FmIcon("description_outlined", size = 16.dp, tint = colors.muted)
                Spacer(Modifier.width(FmSpace.x1))
                Column(Modifier.widthIn(max = 180.dp)) {
                    Text(file.name, maxLines = 1, overflow = TextOverflow.MiddleEllipsis, style = Fm.text.bodySmall.copy(color = colors.text))
                    Text(PromptTextFiles.sizeLabel(file), maxLines = 1, style = Fm.text.labelSmall.copy(color = colors.muted))
                }
                Box(
                    Modifier
                        .size(ControlSize)
                        .clickable { onRemove(file) }
                        .semantics {
                            role = Role.Button
                            contentDescription = "Remove ${file.name}"
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("close", size = 14.dp, tint = colors.muted)
                }
            }
        }
    }
}

@Composable
private fun Thumbnails(
    attachments: List<PromptAttachment>,
    loading: Boolean,
    colors: ComposerPalette,
    onMarkUp: (PromptAttachment) -> Unit,
    onRemove: (PromptAttachment) -> Unit,
) {
    Row(
        Modifier
            .padding(bottom = FmSpace.x2)
            .fillMaxWidth()
            .height(70.dp)
            .horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalAlignment = Alignment.Bottom,
    ) {
        for (attachment in attachments) {
            val image = remember(attachment.id) {
                val options = BitmapFactory.Options().apply { inSampleSize = PhotoPrep.sampleSize(attachment.width, attachment.height, 192) }
                BitmapFactory.decodeByteArray(attachment.bytes, 0, attachment.bytes.size, options)?.asImageBitmap()
            }
            Box(Modifier.size(64.dp)) {
                Box(
                    Modifier
                        .size(64.dp)
                        .clip(RoundedCornerShape(FmRadius.lg))
                        .background(colors.control)
                        .clickable(enabled = !loading) { onMarkUp(attachment) }
                        .semantics {
                            role = Role.Button
                            contentDescription = "Mark up ${attachment.name}"
                        },
                ) {
                    if (image != null) {
                        Image(image, null, Modifier.size(64.dp), contentScale = ContentScale.Crop)
                    }
                }
                Box(
                    Modifier
                        .align(Alignment.TopEnd)
                        .offset(x = 4.dp, y = (-4).dp)
                        .size(22.dp)
                        .clip(CircleShape)
                        .background(colors.primary)
                        .clickable { onRemove(attachment) }
                        .semantics {
                            role = Role.Button
                            contentDescription = "Remove photo"
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    FmIcon("close", size = 14.dp, tint = colors.onPrimary)
                }
            }
        }
        if (loading) {
            Box(
                Modifier.size(64.dp).clip(RoundedCornerShape(FmRadius.lg)).background(colors.control).alpha(0.9f),
                contentAlignment = Alignment.Center,
            ) {
                FmCircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
            }
        }
    }
}
