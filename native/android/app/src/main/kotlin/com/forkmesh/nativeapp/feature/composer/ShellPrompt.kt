package com.forkmesh.nativeapp.feature.composer

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.shell.LaunchFlight
import com.forkmesh.nativeapp.ui.SoundCue
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.layer.drawLayer
import androidx.compose.ui.graphics.rememberGraphicsLayer
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.composer.PromptDraftStore
import com.forkmesh.nativeapp.composer.QuickLaunchFix
import com.forkmesh.nativeapp.composer.QuickLaunchOutcome
import com.forkmesh.nativeapp.composer.ShellPromptTarget
import com.forkmesh.nativeapp.composer.ShellPromptTargets
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.agents.showAgentSetupSheet
import com.forkmesh.nativeapp.feature.localrepos.LocalReposFeature
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
fun BoxScope.ShellPrompt(
    navigator: Navigator,
    open: Boolean,
    onStarted: () -> Unit = {},
    openRun: (Screen) -> Unit = { navigator.push(it) },
    notice: FmStatus? = null,
    onOpenChange: (Boolean) -> Unit,
) {
    val scope = rememberCoroutineScope()
    val quick = remember { ComposerFeature.quick() }
    val drafts = remember { ComposerFeature.drafts }
    val repoContext by ComposerFeature.repoContext.collectAsState()
    var text by remember { mutableStateOf(drafts?.load(PromptDraftStore.SHELL) ?: "") }
    var photos by remember { mutableStateOf<List<PromptAttachment>>(emptyList()) }
    var targetBusy by remember { mutableStateOf(false) }
    val actions = rememberQuickAgentActions(navigator, quick, repoContext, onStarted, openRun)
    val targetTick by ComposerFeature.promptTargetTick.collectAsState()
    val targets = ComposerFeature.promptTargets
    val activeTarget = remember(targetTick) { targets.active }
    val pageTarget = remember(targetTick) { targets.routing }

    LaunchedEffect(open) { if (!open) actions.status.clear() }
    LaunchedEffect(open, repoContext, pageTarget) {
        if (open && pageTarget == null) quick.prepare(repoContext)
    }
    val shared by ComposerFeature.sharedAttachments.collectAsState()
    LaunchedEffect(open, shared) {
        if (open && shared.isNotEmpty()) {
            photos = photos + ComposerFeature.takeSharedAttachments()
        }
        if (open) text = drafts?.load(PromptDraftStore.SHELL) ?: text
    }

    fun updateText(next: String) {
        text = next
        drafts?.save(PromptDraftStore.SHELL, next)
    }

    fun clearSent() {
        updateText("")
        photos = emptyList()
        onOpenChange(false)
    }

    fun sendAgent() {
        actions.send(text, photos) { clearSent() }
    }

    fun sendTarget(to: ShellPromptTarget) {
        if (targetBusy) return
        val message = text.trim()
        val sent = photos
        actions.status.clear()
        scope.launch {
            targetBusy = true
            try {
                val reply = to.submit(message, sent)
                FmSoundCues.play(SoundCue.ARRIVE)
                if (notice != null) {
                    clearSent()
                    notice.show(reply)
                } else {
                    updateText("")
                    photos = emptyList()
                    actions.status.show(reply)
                }
            } catch (thrown: CancellationException) {
                throw thrown
            } catch (thrown: Throwable) {
                FmSoundCues.play(SoundCue.ERROR)
                actions.status.error(ShellPromptTargets.message(thrown))
            } finally {
                targetBusy = false
            }
        }
    }

    fun toggleMode() {
        quick.clearFix()
        if (activeTarget != null) targets.bypass(!targets.bypassed)
    }

    if (!open) return

    DisposableEffect(Unit) { onDispose { LaunchFlight.source = null } }
    BackHandler { onOpenChange(false) }
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.32f))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) { onOpenChange(false) },
    )
    Column(Modifier.align(Alignment.BottomCenter).imePadding()) {
        if (pageTarget != null) {
            PromptComposer(
                text = text,
                onTextChange = ::updateText,
                onSend = { sendTarget(pageTarget) },
                hint = pageTarget.hint,
                busy = targetBusy,
                attachments = if (pageTarget.acceptsPhotos) photos else emptyList(),
                onAttachmentsChanged = if (pageTarget.acceptsPhotos) ({ photos = it }) else null,
                header = if (actions.status.message == null) null else ({ FmInlineStatus(actions.status, Modifier.padding(bottom = FmSpace.x2)) }),
                extraControls = {
                    ComposerIconButton(pageTarget.icon, "${pageTarget.label}. Switch to agent", active = true) { toggleMode() }
                    Spacer(Modifier.width(FmSpace.x2))
                },
                autofocus = true,
                submitOnEnter = true,
                sendDescription = pageTarget.sendDescription,
                padForKeyboard = false,
            )
        } else {
            val shot = rememberGraphicsLayer()
            DisposableEffect(shot) {
                LaunchFlight.capture = { shot.toImageBitmap() }
                onDispose {
                    LaunchFlight.capture = null
                    LaunchFlight.card = null
                }
            }
            Box(
                Modifier
                    .onGloballyPositioned { LaunchFlight.card = it.boundsInRoot() }
                    .drawWithContent {
                        shot.record { this@drawWithContent.drawContent() }
                        drawLayer(shot)
                    },
            ) {
                PromptComposer(
                    text = text,
                    onTextChange = ::updateText,
                    onSend = { sendAgent() },
                    uploadsFiles = true,
                    hint = "New agent · type / for commands",
                    busy = quick.sending,
                    modelLabel = quick.model.label,
                    modelChoices = quick.modelChoices,
                    selectedModel = quick.model,
                    onSelectModel = { picked -> scope.launch { quick.chooseModel(picked) } },
                    attachments = photos,
                    onAttachmentsChanged = { photos = it },
                    header = { QuickActionsHeader(quick, actions) },
                    extraControls = {
                        if (activeTarget != null) {
                            ComposerIconButton("smart_toy_outlined", "Prompt starts an agent. Switch to ${activeTarget.label}", active = true) { toggleMode() }
                            Spacer(Modifier.width(FmSpace.x2))
                        }
                    },
                    autofocus = true,
                    submitOnEnter = false,
                    sendDescription = "Start agent",
                    maxLength = com.forkmesh.nativeapp.agents.AgentQueueApi.MAX_AGENT_PROMPT_CHARS,
                    padForKeyboard = false,
                    onSendBounds = { LaunchFlight.source = it },
                    offersFix = true,
                )
            }
        }
    }
}

@Composable
fun PromptButton(
    tabLabel: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    size: Dp = 44.dp,
    onLongClick: (() -> Unit)? = null,
    running: Int = 0,
) {
    val drafts = remember { ComposerFeature.drafts }
    val hasDraft = remember(tabLabel) { !drafts?.load(PromptDraftStore.SHELL).isNullOrBlank() }
    val accent = Fm.colors.accent
    val glow = accent.copy(alpha = 0.35f + minOf(running, 8) / 8f * 0.25f)
    Box(modifier) {
        Box(
            Modifier
                .size(size)
                .shadow(10.dp, CircleShape, ambientColor = glow, spotColor = glow)
                .clip(CircleShape)
                .background(Brush.verticalGradient(listOf(accent.copy(alpha = 0.95f), accent, accent.copy(alpha = 0.7f))))
                .drawBehind {
                    val stars = minOf(running, 8)
                    for (i in 0 until stars) {
                        val angle = i * 2.399963f
                        val r = this.size.minDimension * (0.22f + 0.05f * (i % 3))
                        val center = Offset(this.size.width / 2 + r * kotlin.math.cos(angle), this.size.height / 2 + r * kotlin.math.sin(angle))
                        drawSparkle(center, this.size.minDimension * 0.07f, Color.White.copy(alpha = 0.85f))
                    }
                }
                .combinedClickable(
                    role = Role.Button,
                    onClick = onClick,
                    onLongClick = onLongClick,
                    onLongClickLabel = onLongClick?.let { "Command palette" },
                )
                .semantics { contentDescription = "New agent" },
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("add", size = size * 0.62f, tint = Color.White)
        }
        if (hasDraft) {
            Box(
                Modifier
                    .align(Alignment.TopEnd)
                    .size(10.dp)
                    .background(Fm.colors.warning, CircleShape),
            )
        }
    }
}

private fun androidx.compose.ui.graphics.drawscope.DrawScope.drawSparkle(center: Offset, radius: Float, color: Color) {
    val waist = radius * 0.22f
    val path = Path().apply {
        moveTo(center.x, center.y - radius)
        quadraticTo(center.x + waist, center.y - waist, center.x + radius, center.y)
        quadraticTo(center.x + waist, center.y + waist, center.x, center.y + radius)
        quadraticTo(center.x - waist, center.y + waist, center.x - radius, center.y)
        quadraticTo(center.x - waist, center.y - waist, center.x, center.y - radius)
        close()
    }
    drawPath(path, color)
}
